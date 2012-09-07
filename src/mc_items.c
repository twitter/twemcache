/*
 * twemcache - Twitter memcached.
 * Copyright (c) 2012, Twitter, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the name of the Twitter nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>

#include <mc_core.h>

extern struct settings settings;

/*
 * We only reposition items in the lru q if they haven't been
 * repositioned in this many seconds. That saves us from churning
 * on frequently-accessed items
 */
#define ITEM_UPDATE_INTERVAL    60

#define ITEM_LRUQ_MAX_TRIES     50

/* 2MB is the maximum response size for 'cachedump' command */
#define ITEM_CACHEDUMP_MEMLIMIT (2 * MB)

pthread_mutex_t cache_lock;                     /* lock protecting lru q and hash */
struct item_tqh item_lruq[SLABCLASS_MAX_IDS];   /* lru q of items */
static uint64_t cas_id;                         /* unique cas id */

/*
 * Returns the next cas id for a new item. Minimum cas value
 * is 1 and the maximum cas value is UINT64_MAX
 */
static uint64_t
item_next_cas(void)
{
    if (settings.use_cas) {
        return ++cas_id;
    }

    return 0ULL;
}

static bool
item_expired(struct item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);

    return (it->exptime > 0 && it->exptime < time_now()) ? true : false;
}

void
item_init(void)
{
    uint8_t i;

    log_debug(LOG_DEBUG, "item hdr size %d", ITEM_HDR_SIZE);

    pthread_mutex_init(&cache_lock, NULL);

    for (i = SLABCLASS_MIN_ID; i <= SLABCLASS_MAX_ID; i++) {
        TAILQ_INIT(&item_lruq[i]);
    }

    cas_id = 0ULL;
}

void
item_deinit(void)
{
}

/*
 * Get the slab that contains this item.
 */
struct slab *
item_2_slab(struct item *it)
{
    struct slab *slab;

    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT(it->offset < settings.slab_size);

    slab = (struct slab *)((uint8_t *)it - it->offset);

    ASSERT(slab->magic == SLAB_MAGIC);

    return slab;
}

static void
item_acquire_refcount(struct item *it)
{
    ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
    ASSERT(it->magic == ITEM_MAGIC);

    it->refcount++;
    slab_acquire_refcount(item_2_slab(it));
}

static void
item_release_refcount(struct item *it)
{
    ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT(it->refcount > 0);

    it->refcount--;
    slab_release_refcount(item_2_slab(it));
}

void
item_hdr_init(struct item *it, uint32_t offset, uint8_t id)
{
    ASSERT(offset >= SLAB_HDR_SIZE && offset < settings.slab_size);

    it->magic = ITEM_MAGIC;
    it->offset = offset;
    it->id = id;
    it->refcount = 0;
    it->flags = 0;
}

/*
 * Add an item to the tail of the lru q.
 *
 * Lru q is sorted in ascending time order - oldest to most recent. So
 * enqueuing at item to the tail of the lru q requires us to update its
 * last access time atime.
 *
 * The allocated flag indicates whether the item being re-linked is a newly
 * allocated or not. This is useful for updating the slab lruq, which can
 * choose to update only when a new item has been allocated (write-only) or
 * the opposite (read-only), or on both occasions (access-based).
 */
static void
item_link_q(struct item *it, bool allocated)
{
    uint8_t id = it->id;

    ASSERT(id >= SLABCLASS_MIN_ID && id <= SLABCLASS_MAX_ID);
    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT((it->flags & ITEM_SLABBED) == 0);

    log_debug(LOG_VVERB, "link q it '%.*s' at offset %"PRIu32" with flags "
              "%02x id %"PRId8"", it->nkey, item_key(it), it->offset,
              it->flags, it->id);

    it->atime = time_now();
    TAILQ_INSERT_TAIL(&item_lruq[id], it, i_tqe);

    slab_lruq_touch(item_2_slab(it), allocated);

    stats_slab_incr(id, item_curr);
    stats_slab_incr_by(id, data_curr, item_size(it));
    stats_slab_incr_by(id, data_value_curr, it->nbyte);
}

/*
 * Remove the item from the lru q
 */
static void
item_unlink_q(struct item *it)
{
    uint8_t id = it->id;

    ASSERT(id >= SLABCLASS_MIN_ID && id <= SLABCLASS_MAX_ID);
    ASSERT(it->magic == ITEM_MAGIC);

    log_debug(LOG_VVERB, "unlink q it '%.*s' at offset %"PRIu32" with flags "
              "%02x id %"PRId8"", it->nkey, item_key(it), it->offset,
              it->flags, it->id);

    TAILQ_REMOVE(&item_lruq[id], it, i_tqe);

    stats_slab_decr(id, item_curr);
    stats_slab_decr_by(id, data_curr, item_size(it));
    stats_slab_decr_by(id, data_value_curr, it->nbyte);
}

/*
 * Make an item with zero refcount available for reuse by unlinking
 * it from the lru q and hash.
 *
 * Don't free the item yet because that would make it unavailable
 * for reuse.
 */
void
item_reuse(struct item *it)
{
    ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT((it->flags & ITEM_SLABBED) == 0);
    ASSERT((it->flags & ITEM_LINKED) != 0);
    ASSERT(it->refcount == 0);

    it->flags &= ~ITEM_LINKED;

    assoc_delete(item_key(it), it->nkey);
    item_unlink_q(it);

    stats_slab_incr(it->id, item_remove);
    stats_slab_settime(it->id, item_reclaim_ts, time_now());

    log_debug(LOG_VERB, "reuse %s it '%.*s' at offset %"PRIu32" with id "
              "%"PRIu8"", item_expired(it) ? "expired" : "evicted",
              it->nkey, item_key(it), it->offset, it->id);
}

/*
 * Find an unused (unreferenced) item from lru q.
 *
 * First try to find an expired item from the lru Q of item's slab
 * class; if all items are unexpired, return the one that is the
 * least recently used.
 *
 * We bound the search for an expired item in lru q, by only
 * traversing the oldest ITEM_LRUQ_MAX_TRIES items.
 */
static struct item *
item_get_from_lruq(uint8_t id)
{
    struct item *it;  /* expired item */
    struct item *uit; /* unexpired item */
    uint32_t tries;

    if (!settings.use_lruq) {
        return NULL;
    }

    for (tries = ITEM_LRUQ_MAX_TRIES, it = TAILQ_FIRST(&item_lruq[id]),
         uit = NULL;
         it != NULL && tries > 0;
         tries--, it = TAILQ_NEXT(it, i_tqe)) {

        if (it->refcount != 0) {
            log_debug(LOG_VVERB, "skip it '%.*s' at offset %"PRIu32" with "
                      "flags %02x id %"PRId8" refcount %"PRIu16"", it->nkey,
                      item_key(it), it->offset, it->flags, it->id,
                      it->refcount);
            continue;
        }

        if (item_expired(it)) {
            /* first expired item wins */
            return it;
        } else if (uit == NULL) {
            /* otherwise, get the lru unexpired item */
            uit = it;
        }
    }

    return uit;
}

uint8_t item_slabid(uint8_t nkey, uint32_t nbyte)
{
    size_t ntotal;
    uint8_t id;

    ntotal = item_ntotal(nkey, nbyte, settings.use_cas);

    id = slab_id(ntotal);
    if (id == SLABCLASS_INVALID_ID) {
        log_debug(LOG_NOTICE, "slab class id out of range with %"PRIu8" bytes "
                  "key, %"PRIu32" bytes value and %zu item chunk size", nkey,
                  nbyte, ntotal);
    }

    return id;
}

/*
 * Allocate an item. We allocate an item either by -
 *  1. Reusing an expired item from the lru Q of an item's slab class. Or,
 *  2. Consuming the next free item from slab of the item's an slab class
 *
 * On success we return the pointer to the allocated item. The returned item
 * is refcounted so that it is not deleted under callers nose. It is the
 * callers responsibilty to release this refcount when the item is inserted
 * into the hash + lru q or freed.
 */
static struct item *
_item_alloc(uint8_t id, char *key, uint8_t nkey, uint32_t dataflags, rel_time_t
            exptime, uint32_t nbyte)
{
    struct item *it;  /* item */
    struct item *uit; /* unexpired lru item */

    ASSERT(id >= SLABCLASS_MIN_ID && id <= SLABCLASS_MAX_ID);

    /*
     * We try to obtain an item in the following order:
     *  1)  by acquiring an expired item;
     *  2)  by getting a free slot from the last slab in current class;
     *  3)  by evicting a slab, if slab eviction(s) are enabled;
     *  4)  by evicting an item, if item lru eviction is enabled.
     */
    it = item_get_from_lruq(id); /* expired / unexpired lru item */

    if (it != NULL && item_expired(it)) {
        /* 1) this is an expired item, always use it */
        stats_slab_incr(id, item_expire);
        stats_slab_settime(id, item_expire_ts, it->exptime);

        item_reuse(it);
        goto done;
    }

    uit = (settings.evict_opt & EVICT_LRU)? it : NULL; /* keep if can be used */

    it = slab_get_item(id);
    if (it != NULL) {
        /* 2) or 3a) either we allow random eviction a free item is found */
        goto done;
    }

    if (uit != NULL) {
        /* 3b) this is an lru item and we can reuse it */
        it = uit;
        stats_slab_incr(id, item_evict);
        stats_slab_settime(id, item_evict_ts, time_now());

        item_reuse(it);
        goto done;
    }

    log_warn("server error on allocating item in slab %"PRIu8, id);

    stats_thread_incr(server_error);

    return NULL;

done:

    ASSERT(it->id == id);
    ASSERT((it->flags & (ITEM_LINKED | ITEM_SLABBED)) == 0);
    ASSERT(it->offset != 0);
    ASSERT(it->refcount == 0);

    item_acquire_refcount(it);

    it->flags = settings.use_cas ? ITEM_CAS : 0;
    it->dataflags = dataflags;
    it->nbyte = nbyte;
    it->exptime = exptime;
    it->nkey = nkey;
    memcpy(item_key(it), key, nkey);

    stats_slab_incr(id, item_acquire);

    log_debug(LOG_VERB, "alloc it '%.*s' at offset %"PRIu32" with id %"PRIu8
              " expiry %u refcount %"PRIu16"", it->nkey, item_key(it),
              it->offset, it->id, it->exptime, it->refcount);

    return it;
}

struct item *
item_alloc(uint8_t id, char *key, size_t nkey, uint32_t flags,
           rel_time_t exptime, uint32_t nbyte)
{
    struct item *it;

    pthread_mutex_lock(&cache_lock);
    it = _item_alloc(id, key, nkey, flags, exptime, nbyte);
    pthread_mutex_unlock(&cache_lock);

    return it;
}

static void
item_free(struct item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);
    slab_put_item(it);
}

/*
 * Link an item into the hash table and lru q
 */
static void
_item_link(struct item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT((it->flags & (ITEM_LINKED | ITEM_SLABBED)) == 0);

    log_debug(LOG_DEBUG, "link it '%.*s' at offset %"PRIu32" with flags "
              "%02x id %"PRId8"", it->nkey, item_key(it), it->offset,
              it->flags, it->id);

    it->flags |= ITEM_LINKED;
    item_set_cas(it, item_next_cas());

    assoc_insert(it);
    item_link_q(it, true);
}

/*
 * Unlinks an item from the lru q and hash table. Free an unlinked
 * item if it's refcount is zero.
 */
static void
_item_unlink(struct item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT((it->flags & ITEM_LINKED) != 0);

    log_debug(LOG_DEBUG, "unlink it '%.*s' at offset %"PRIu32" with flags "
              "%02x id %"PRId8"", it->nkey, item_key(it), it->offset,
              it->flags, it->id);

    if ((it->flags & ITEM_LINKED) != 0) {
        it->flags &= ~ITEM_LINKED;

        assoc_delete(item_key(it), it->nkey);

        item_unlink_q(it);

        if (it->refcount == 0) {
            item_free(it);
        }
    }
}

/*
 * Decrement the refcount on an item. Free an unliked item if its refcount
 * drops to zero.
 */
static void
_item_remove(struct item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT((it->flags & ITEM_SLABBED) == 0);

    log_debug(LOG_DEBUG, "remove it '%.*s' at offset %"PRIu32" with flags "
              "%02x id %"PRId8" refcount %"PRIu16"", it->nkey, item_key(it),
              it->offset, it->flags, it->id, it->refcount);

    if (it->refcount != 0) {
        item_release_refcount(it);
    }

    if (it->refcount == 0 && (it->flags & ITEM_LINKED) == 0) {
        item_free(it);
    }
}

void
item_remove(struct item *it)
{
    pthread_mutex_lock(&cache_lock);
    _item_remove(it);
    pthread_mutex_unlock(&cache_lock);
}

/*
 * Unlink an item and remove it (if its recount drops to zero).
 */
void
item_delete(struct item *it)
{
    pthread_mutex_lock(&cache_lock);
    _item_unlink(it);
    _item_remove(it);
    pthread_mutex_unlock(&cache_lock);
}

/*
 * Touch the item by moving it to the tail of lru q only if it wasn't
 * touched ITEM_UPDATE_INTERVAL secs back.
 */
static void
_item_touch(struct item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT((it->flags & ITEM_SLABBED) == 0);

    if (it->atime >= (time_now() - ITEM_UPDATE_INTERVAL)) {
        return;
    }

    log_debug(LOG_VERB, "update it '%.*s' at offset %"PRIu32" with flags "
              "%02x id %"PRId8"", it->nkey, item_key(it), it->offset,
              it->flags, it->id);

    ASSERT((it->flags & ITEM_LINKED) != 0);
    if ((it->flags & ITEM_LINKED) != 0) {
        item_unlink_q(it);
        item_link_q(it, false);
    }
}

void
item_touch(struct item *it)
{
    if (it->atime >= (time_now() - ITEM_UPDATE_INTERVAL)) {
        return;
    }

    pthread_mutex_lock(&cache_lock);
    _item_touch(it);
    pthread_mutex_unlock(&cache_lock);
}

/*
 * Replace one item with another in the hash table and lru q.
 */
static void
_item_replace(struct item *it, struct item *nit)
{
    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT((it->flags & ITEM_SLABBED) == 0);

    ASSERT(nit->magic == ITEM_MAGIC);
    ASSERT((nit->flags & ITEM_SLABBED) == 0);

    log_debug(LOG_VERB, "replace it '%.*s' at offset %"PRIu32" id %"PRIu8" "
              "with one at offset %"PRIu32" id %"PRIu8"", it->nkey,
              item_key(it), it->offset, it->id, nit->offset, nit->id);

    _item_unlink(it);
    _item_link(nit);
}

static char *
_item_cache_dump(uint8_t id, uint32_t limit, uint32_t *bytes)
{
    const size_t memlimit = ITEM_CACHEDUMP_MEMLIMIT;
    char *buffer;
    uint32_t bufcurr;
    struct item *it;
    uint32_t len;
    uint32_t shown = 0;
    char key_temp[KEY_MAX_LEN + 1];
    char temp[KEY_MAX_LEN * 2];

    buffer = mc_alloc(memlimit);
    if (buffer == NULL) {
        return NULL;
    }

    for (bufcurr = 0, it = TAILQ_FIRST(&item_lruq[id]);
         it != NULL && (limit == 0 || shown < limit);
         it = TAILQ_NEXT(it, i_tqe)) {

        ASSERT(it->nkey <= KEY_MAX_LEN);
        /* copy the key since it may not be null-terminated in the struct */
        strncpy(key_temp, item_key(it), it->nkey);
        key_temp[it->nkey] = '\0'; /* terminate */
        len = snprintf(temp, sizeof(temp), "ITEM %s [%d b; %lu s]\r\n",
                       key_temp, it->nbyte,
                       (unsigned long)it->exptime + time_started());
        if (len >= sizeof(temp)) {
            log_debug(LOG_WARN, "item log was truncated during cache dump");
        }
        if (bufcurr + len + (sizeof("END\r\n") - 1) > memlimit) {
            break;
        }
        memcpy(buffer + bufcurr, temp, len);
        bufcurr += len;
        shown++;
    }

    memcpy(buffer + bufcurr, "END\r\n", sizeof("END\r\n") - 1);
    bufcurr += sizeof("END\r\n") - 1;

    *bytes = bufcurr;

    return buffer;
}

char *
item_cache_dump(uint8_t id, uint32_t limit, uint32_t *bytes)
{
    char *ret;

    pthread_mutex_lock(&cache_lock);
    ret = _item_cache_dump(id, limit, bytes);
    pthread_mutex_unlock(&cache_lock);

    return ret;
}

/*
 * Return an item if it hasn't been marked as expired, lazily expiring
 * item as-and-when needed
 *
 * When a non-null item is returned, it's the callers responsibily to
 * release refcount on the item
 */
static struct item *
_item_get(const char *key, size_t nkey)
{
    struct item *it;

    it = assoc_find(key, nkey);
    if (it == NULL) {
        log_debug(LOG_VERB, "get it '%.*s' not found", nkey, key);
        return NULL;
    }

    if (it->exptime != 0 && it->exptime <= time_now()) {
        _item_unlink(it);
        stats_slab_incr(it->id, item_expire);
        stats_slab_settime(it->id, item_reclaim_ts, time_now());
        stats_slab_settime(it->id, item_expire_ts, it->exptime);
        log_debug(LOG_VERB, "get it '%.*s' expired and nuked", nkey, key);
        return NULL;
    }

    if (settings.oldest_live != 0 && settings.oldest_live <= time_now() &&
        it->atime <= settings.oldest_live) {
        _item_unlink(it);
        stats_slab_incr(it->id, item_evict);
        stats_slab_settime(it->id, item_evict_ts, time_now() );
        log_debug(LOG_VERB, "it '%.*s' nuked", nkey, key);
        return NULL;
    }

    item_acquire_refcount(it);

    log_debug(LOG_VERB, "get it '%.*s' found at offset %"PRIu32" with flags "
              "%02x id %"PRIu8" refcount %"PRIu32"", it->nkey, item_key(it),
              it->offset, it->flags, it->id);


    return it;
}

struct item *
item_get(const char *key, size_t nkey)
{
    struct item *it;

    pthread_mutex_lock(&cache_lock);
    it = _item_get(key, nkey);
    pthread_mutex_unlock(&cache_lock);

    return it;
}

/*
 * Flushes expired items after a "flush_all" call. Expires items that
 * are more recent than the oldest_live setting
 */
static void
_item_flush_expired(void)
{
    uint8_t i;
    struct item *it, *next;

    if (settings.oldest_live == 0) {
        return;
    }

    for (i = SLABCLASS_MIN_ID; i <= SLABCLASS_MAX_ID; i++) {
        /*
         * Lru q is sorted in ascending time order -- oldest to most recent.
         * Since time is computed at granularity of one sec, we would like
         * to flush items which are accessed during this one sec. So we
         * proactively flush most recent ones in a given queue until we hit
         * an item older than oldest_live. Older items in this queue are then
         * lazily expired by oldest_live check in item_get.
         */
        TAILQ_FOREACH_REVERSE_SAFE(it, &item_lruq[i], item_tqh, i_tqe, next) {
            ASSERT((it->flags & ITEM_SLABBED) == 0);

            if (it->atime < settings.oldest_live) {
                break;
            }

            _item_unlink(it);

            stats_slab_incr(it->id, item_evict);
            stats_slab_settime(it->id, item_evict_ts, time_now());
        }
    }
}

void
item_flush_expired(void)
{
    pthread_mutex_lock(&cache_lock);
    _item_flush_expired();
    pthread_mutex_unlock(&cache_lock);
}

/*
 * Store an item in the cache according to the semantics of one of the
 * update commands - {set, add, replace, append, prepend, cas}
 */
static item_store_result_t
_item_store(struct item *it, req_type_t type, struct conn *c)
{
    item_store_result_t result;  /* item store result */
    bool store_it;               /* store item ? */
    char *key;                   /* item key */
    struct item *oit, *nit;      /* old (existing) item & new item */
    uint8_t id;                  /* slab id */
    uint32_t total_nbyte;

    result = NOT_STORED;
    store_it = true;

    key = item_key(it);
    nit = NULL;
    oit = _item_get(key, it->nkey);
    if (oit == NULL) {
        switch (type) {
        case REQ_SET:
            stats_slab_incr(it->id, set_success);
            break;

        case REQ_ADD:
            stats_slab_incr(it->id, add_success);
            break;

        case REQ_REPLACE:
            stats_thread_incr(replace_miss);
            store_it = false;
            break;

        case REQ_APPEND:
            stats_thread_incr(append_miss);
            store_it = false;
            break;

        case REQ_PREPEND:
            stats_thread_incr(prepend_miss);
            store_it = false;
            break;

        case REQ_CAS:
            stats_thread_incr(cas_miss);
            result = NOT_FOUND;
            store_it = false;
            break;

        default:
            NOT_REACHED();
        }
    } else {
        switch (type) {
        case REQ_SET:
            stats_slab_incr(it->id, set_success);
            break;

        case REQ_ADD:
            stats_thread_incr(add_exist);
            /*
             * Add only adds a non existing item. However we promote the
             * existing item to head of the lru q
             */
            _item_touch(oit);
            store_it = false;
            break;

        case REQ_REPLACE:
            stats_slab_incr(oit->id, replace_hit);
            stats_slab_incr(it->id, replace_success);
            break;

        case REQ_APPEND:
            stats_slab_incr(oit->id, append_hit);

            /*
             * Alloc new item - nit to hold both it and oit
             */
            total_nbyte = oit->nbyte + it->nbyte;
            id = item_slabid(oit->nkey, total_nbyte);
            if (id == SLABCLASS_INVALID_ID) {
                /* FIXME: logging client error but not sending CLIENT ERROR
                 * to the client because we are inside the item module, which
                 * technically shouldn't directly handle commands. There is not
                 * a proper return status to indicate such an error.
                 * This can only be fixed by moving the command-aware logic
                 * into a separate module.
                 */
                log_debug(LOG_NOTICE, "client error on c %d for req of type %d"
                          " with key size %"PRIu8" and value size %"PRIu32,
                          c->sd, c->req_type, oit->nkey, total_nbyte);
            }
            nit = _item_alloc(id, key, oit->nkey, oit->dataflags, oit->exptime,
                              total_nbyte);
            if (nit == NULL) {
                store_it = false;
                break;
            }

            stats_slab_incr(nit->id, append_success);

            /*
             * Copy total_nbyte of data from it and oit to nit, where
             *  - append =  nit <- [oit, it]
             */
            memcpy(item_data(nit), item_data(oit), oit->nbyte);
            memcpy(item_data(nit) + oit->nbyte, item_data(it), it->nbyte);
            it = nit;

            break;

        case REQ_PREPEND:
            stats_slab_incr(oit->id, prepend_hit);

            /*
             * Alloc new item - nit to hold both it and oit
             */
            total_nbyte = oit->nbyte + it->nbyte;
            id = item_slabid(oit->nkey, total_nbyte);
            if (id == SLABCLASS_INVALID_ID) {
                log_debug(LOG_NOTICE, "client error on c %d for req of type %d"
                          " with key size %"PRIu8" and value size %"PRIu32,
                          c->sd, c->req_type, oit->nkey, total_nbyte);
            }
            nit = _item_alloc(id, key, oit->nkey, oit->dataflags, oit->exptime,
                              total_nbyte);
            if (nit == NULL) {
                store_it = false;
                break;
            }

            stats_slab_incr(nit->id, prepend_success);
            /*
             * Copy total_byte of data from it and oit to nit, where
             *  - prepend = nit <- [it, oit]
             */

            memcpy(item_data(nit), item_data(it), it->nbyte);
            memcpy(item_data(nit) + it->nbyte, item_data(oit), oit->nbyte);
            it = nit;

            break;

        case REQ_CAS:
            if (item_cas(it) != item_cas(oit)) {
                log_debug(LOG_DEBUG, "cas mismatch %"PRIu64" != %"PRIu64 "on "
                          "it '%.*s'", item_cas(oit), item_cas(it), it->nkey,
                          item_key(it));
                stats_slab_incr(oit->id, cas_badval);
                result = EXISTS;
                break;
            }

            stats_slab_incr(oit->id, cas_hit);
            stats_slab_incr(it->id, cas_success);
            break;

        default:
            NOT_REACHED();
        }
    }

    if (result == NOT_STORED && store_it) {
        if (oit != NULL) {
            _item_replace(oit, it);
        } else {
            _item_link(it);
        }
        result = STORED;
    }

    /* release our reference, if any */
    if (oit != NULL) {
        _item_remove(oit);
    }

    if (nit != NULL) {
        _item_remove(nit);
    }

    return result;
}

item_store_result_t
item_store(struct item *it, req_type_t type, struct conn *c)
{
    item_store_result_t ret;

    pthread_mutex_lock(&cache_lock);
    ret = _item_store(it, type, c);
    pthread_mutex_unlock(&cache_lock);

    return ret;
}

/*
 * Add a delta value (positive or negative) to an item.
 */
static item_delta_result_t
_item_add_delta(struct conn *c, char *key, size_t nkey, bool incr,
                int64_t delta, char *buf)
{
    int res;
    char *ptr;
    uint64_t value;
    struct item *it;

    it = _item_get(key, nkey);
    if (it == NULL) {
        return DELTA_NOT_FOUND;
    }

    ptr = item_data(it);

    if (!mc_strtoull(ptr, &value)) {
        _item_remove(it);
        return DELTA_NON_NUMERIC;
    }

    if (incr) {
        value += delta;
    } else if (delta > value) {
        value = 0;
    } else {
        value -= delta;
    }

    if (incr) {
        stats_slab_incr(it->id, incr_hit);
    } else {
        stats_slab_incr(it->id, decr_hit);
    }

    res = snprintf(buf, INCR_MAX_STORAGE_LEN, "%"PRIu64, value);
    ASSERT(res < INCR_MAX_STORAGE_LEN);
    if (res > it->nbyte) { /* need to realloc */
        struct item *new_it;
        uint8_t id;

        id = item_slabid(it->nkey, res);
        if (id == SLABCLASS_INVALID_ID) {
            log_debug(LOG_NOTICE, "client error on c %d for req of type %d"
            " with key size %"PRIu8" and value size %"PRIu32, c->sd,
            c->req_type, it->nkey, res);
        }

        new_it = _item_alloc(id, item_key(it), it->nkey, it->dataflags,
                             it->exptime, res);
        if (new_it == NULL) {
            _item_remove(it);
            return DELTA_EOM;
        }
        if (incr) {
            stats_slab_incr(new_it->id, incr_success);
        } else {
            stats_slab_incr(new_it->id, decr_success);
        }

        memcpy(item_data(new_it), buf, res);
        _item_replace(it, new_it);
        _item_remove(new_it);
    } else {
        /*
         * Replace in-place - when changing the value without replacing
         * the item, we need to update the CAS on the existing item
         */
        if (incr) {
            stats_slab_incr(it->id, incr_success);
        } else {
            stats_slab_incr(it->id, decr_success);
        }

        item_set_cas(it, item_next_cas());

        memcpy(item_data(it), buf, res);
        memset(item_data(it) + res, ' ', it->nbyte - res);
    }

    _item_remove(it);

    return DELTA_OK;
}

item_delta_result_t
item_add_delta(struct conn *c, char *key, size_t nkey, int incr,
               int64_t delta, char *buf)
{
    item_delta_result_t ret;

    pthread_mutex_lock(&cache_lock);
    ret = _item_add_delta(c, key, nkey, incr, delta, buf);
    pthread_mutex_unlock(&cache_lock);

    return ret;
}
