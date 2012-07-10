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

#include <mc_core.h>

extern struct settings settings;
extern pthread_mutex_t cache_lock;

struct slabinfo {
    uint8_t     *base;       /* prealloc base */
    uint8_t     *curr;       /* prealloc start */

    uint32_t    nslab;       /* # slab allocated */
    uint32_t    max_nslab;   /* max # slab allowed */
    struct slab **slabtable; /* table of all slabs */
};

struct slabclass slabclass[SLABCLASS_MAX_IDS];  /* collection of slabs bucketed by slabclass */
uint8_t slabclass_max_id;                       /* maximum slabclass id */
static struct slabinfo slabinfo;                /* info of all allocated slabs */
pthread_mutex_t slab_lock;                      /* lock protecting slabclass and slabinfo */

#define SLAB_RAND_MAX_TRIES 50

/*
 * Return the usable space for item sized chunks that would be carved out
 * of a given slab.
 */
size_t
slab_size(void)
{
    return settings.slab_size - SLAB_HDR_SIZE;
}

void
slab_print(void)
{
    uint8_t id;
    struct slabclass *p;

    loga("slab size %zu, slab hdr size %zu, item hdr size %zu, "
         "item chunk size %zu, total memory %zu", settings.slab_size,
         SLAB_HDR_SIZE, ITEM_HDR_SIZE, settings.chunk_size,
         settings.maxbytes);

    for (id = SLABCLASS_MIN_ID; id <= slabclass_max_id; id++) {
        p = &slabclass[id];

        loga("class %3"PRId8": items %7"PRIu32"  size %7zu  data %7zu  "
             "slack %7zu", id, p->nitem, p->size, p->size - ITEM_HDR_SIZE,
             slab_size() - p->nitem * p->size);
    }
}

void
slab_acquire_refcount(struct slab *slab)
{
    ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
    ASSERT(slab->magic == SLAB_MAGIC);
    slab->refcount++;
}

void
slab_release_refcount(struct slab *slab)
{
    ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
    ASSERT(slab->magic == SLAB_MAGIC);
    ASSERT(slab->refcount > 0);
    slab->refcount--;
}

/*
 * Get the idx^th item with a given size from the slab.
 */
static struct item *
slab_2_item(struct slab *slab, uint32_t idx, size_t size)
{
    struct item *it;
    uint32_t offset = idx * size;

    ASSERT(slab->magic == SLAB_MAGIC);
    ASSERT(offset < settings.slab_size);

    it = (struct item *)((uint8_t *)slab->data + offset);

    return it;
}

/*
 * Return the id of the slab which can store an item of a given size.
 *
 * Return SLABCLASS_INVALID_ID, for large items which cannot be stored in
 * any of the configured slabs.
 */
uint8_t
slab_id(size_t size)
{
    uint8_t id, imin, imax;

    ASSERT(size != 0);

    /* binary search */
    imin = SLABCLASS_MIN_ID;
    imax = slabclass_max_id;
    while (imax >= imin) {
        id = (imin + imax) / 2;
        if (size > slabclass[id].size) {
            imin = id + 1;
        } else if (id > SLABCLASS_MIN_ID && size <= slabclass[id - 1].size) {
            imax = id - 1;
        } else {
            break;
        }
    }

    if (imin > imax) {
        /* size too big for any slab */
        return SLABCLASS_INVALID_ID;
    }

    return id;
}

/*
 * Initialize all slabclasses.
 *
 * Every slabclass is a collection of slabs of fixed size specified by
 * --slab-size. A single slab is a collection of contiguous, equal sized
 * item chunks of a given size specified by the settings.profile array
 */
static void
slab_init_slabclass(void)
{
    uint8_t id;      /* slabclass id */
    size_t *profile; /* slab profile */

    profile = settings.profile;
    slabclass_max_id = settings.profile_last_id;

    ASSERT(slabclass_max_id <= SLABCLASS_MAX_ID);

    for (id = SLABCLASS_MIN_ID; id <= slabclass_max_id; id++) {
        struct slabclass *p; /* slabclass */
        uint32_t nitem;      /* # item per slabclass */
        size_t item_sz;      /* item size */

        nitem = slab_size() / profile[id];
        item_sz = profile[id];
        p = &slabclass[id];

        p->nitem = nitem;
        p->size = item_sz;

        p->nfree_itemq = 0;
        TAILQ_INIT(&p->free_itemq);

        p->nslabq = 0;
        TAILQ_INIT(&p->slabq);

        p->nfree_item = 0;
        p->free_item = NULL;
    }
}

/*
 * Initialize the slabclass and slabinfo.
 *
 * When prelloc is true, the slab allocator allocates all the slabs
 * upfront. Otherwise, the slabs are allocated on demand. But once
 * a slab is allocated, it is never free, though a slab could be
 * reused on eviction.
 */
rstatus_t
slab_init(void)
{
    struct slabinfo *sinfo = &slabinfo;

    sinfo->nslab = 0;
    sinfo->max_nslab = settings.maxbytes / settings.slab_size;

    sinfo->base = NULL;
    if (settings.prealloc) {
        sinfo->base = mc_alloc(sinfo->max_nslab * settings.slab_size);
        if (sinfo->base == NULL) {
            return MC_ENOMEM;
        }

        log_debug(LOG_INFO, "pre-allocated %zu bytes for %"PRIu32" slabs",
                  settings.maxbytes, sinfo->max_nslab);
    }
    sinfo->curr = sinfo->base;

    sinfo->slabtable = mc_alloc(sizeof(*sinfo->slabtable) * sinfo->max_nslab);
    if (sinfo->slabtable == NULL) {
        return MC_ENOMEM;
    }

    log_debug(LOG_VVERB, "created slab table with %zu entries",
              sinfo->max_nslab);
    pthread_mutex_init(&slab_lock, NULL);

    slab_init_slabclass();

    return MC_OK;
}

void
slab_deinit(void)
{
}

static void
slab_hdr_init(struct slab *slab, uint8_t id)
{
    ASSERT(id >= SLABCLASS_MIN_ID && id <= slabclass_max_id);

    slab->magic = SLAB_MAGIC;
    slab->id = id;
    slab->unused = 0;
    slab->refcount = 0;
}

/*
 * Get a raw slab from the slab pool.
 */
static struct slab *
slab_get_new(void)
{
    struct slabinfo *sinfo = &slabinfo;
    struct slab *slab;

    if (sinfo->nslab >= sinfo->max_nslab) {
        return NULL;
    }

    if (settings.prealloc) {
        slab = (struct slab *)sinfo->curr;
        sinfo->curr += settings.slab_size;
    } else {
        slab = mc_alloc(settings.slab_size);
        if (slab == NULL) {
            return NULL;
        }
    }

    sinfo->slabtable[sinfo->nslab] = slab;
    sinfo->nslab++;

    log_debug(LOG_VERB, "new slab %p allocated at pos %u", slab,
              sinfo->nslab - 1);

    return slab;
}

/*
 * Get a random slab from all active slabs. All the items in the candidate
 * slab must be evicted before the slab can be returned. This means that the
 * items that are carved out of the slab must either be deleted from their
 * a) hash + lru Q, or b) free Q. The candidate slab itself must also be
 * delinked from its respective slab pool so that it is available for reuse.
 *
 * Note that the slabtable enables us to have O(1) lookup for every slab in
 * the system. The inserts into the table are just appends - O(1) and there
 * are no deletes from the slabtable. This two constraints allows us to keep
 * our random choice uniform. Eviction, however is O(#items/slab).
 */
static struct slab *
slab_get_evicted(void)
{
    struct slabinfo *sinfo = &slabinfo;
    struct slabclass *p;
    struct slab *slab;
    struct item *it;
    uint32_t tries;
    uint32_t rand_idx;
    uint32_t i;

    tries = SLAB_RAND_MAX_TRIES;
    do {
        rand_idx = (uint32_t)rand() % sinfo->nslab;
        slab = sinfo->slabtable[rand_idx];
    } while (--tries > 0 && slab->refcount != 0);

    if (tries == 0) {
        /* all randomly chosen slabs are in use */
        return NULL;
    }

    log_debug(LOG_DEBUG, "evicting slab %p with id %u from pos %u", slab,
              slab->id, rand_idx);

    p = &slabclass[slab->id];

    /* candidate slab is also the current slab */
    if (p->free_item != NULL && slab == item_2_slab(p->free_item)) {
        p->nfree_item = 0;
        p->free_item = NULL;
    }

    /* delete slab items either from hash + lru Q or free Q */
    for (i = 0; i < p->nitem; i++) {
        it = slab_2_item(slab, i, p->size);

        ASSERT(it->magic == ITEM_MAGIC);
        ASSERT(it->refcount == 0);
        ASSERT(it->offset != 0);

        if ((it->flags & ITEM_LINKED) != 0) {
            item_reuse(it);
        } else if ((it->flags & ITEM_SLABBED) != 0) {
            ASSERT(slab == item_2_slab(it));
            ASSERT(!TAILQ_EMPTY(&p->free_itemq));

            it->flags &= ~ITEM_SLABBED;

            ASSERT(p->nfree_itemq > 0);
            p->nfree_itemq--;
            TAILQ_REMOVE(&p->free_itemq, it, i_tqe);
            stats_slab_decr(slab->id, item_free);
        }
    }

    /* unlink the slab from its class */
    ASSERT(p->nslabq > 0);
    p->nslabq--;
    TAILQ_REMOVE(&p->slabq, slab, s_tqe);

    stats_slab_incr(slab->id, slab_evict);
    stats_slab_decr(slab->id, slab_curr);
    stats_slab_settime(slab->id, slab_evict_ts, time_now());

    return slab;
}

/*
 * Get a slab. We return a slab either from the:
 * 1. slab pool, if not empty. or,
 * 2. evict an active slab and return that instead.
 */
static rstatus_t
slab_get(uint8_t id, bool evict)
{
    struct slab *slab;
    struct slabclass *p;
    struct item *it;
    uint32_t offset;
    uint32_t i;

    stats_slab_incr(id, slab_req);
    stats_slab_settime(id, slab_req_ts, time_now());

    p = &slabclass[id];

    ASSERT(p->free_item == NULL);
    ASSERT(TAILQ_EMPTY(&p->free_itemq));

    slab = slab_get_new();
    if (slab == NULL) {
        if (evict) {
            /* if eviction is allowed, we should succeed */
            slab = slab_get_evicted();
            if (slab == NULL) {
                stats_slab_incr(id, slab_error);
                stats_slab_settime(id, slab_error_ts, time_now());
                return MC_ENOMEM;
            }
        } else {
            stats_slab_incr(id, slab_error);
            stats_slab_settime(id, slab_error_ts, time_now());
            return MC_ENOMEM;
        }
    } else {
        stats_slab_settime(id, slab_new_ts, time_now());
    }

    /* once reached here we have one more slab to the current slabclass */
    stats_slab_incr(id, slab_alloc);
    stats_slab_incr(id, slab_curr);
    stats_slab_settime(id, slab_alloc_ts, time_now());

    /* initialize slab header */
    slab_hdr_init(slab, id);

    p->nslabq++;
    TAILQ_INSERT_TAIL(&p->slabq, slab, s_tqe);

    /* initialize all slab items */
    for (i = 0; i < p->nitem; i++) {
        it = slab_2_item(slab, i, p->size);
        offset = (uint32_t)((uint8_t *)it - (uint8_t *)slab);
        item_hdr_init(it, offset, id);
    }

    /* make this slab as the current slab */
    p->nfree_item = p->nitem;
    p->free_item = (struct item *)&slab->data[0];

    return MC_OK;
}

/*
 * Get an item from the item free q of the given slab with id.
 */
static struct item *
slab_get_item_from_freeq(uint8_t id)
{
    struct slabclass *p; /* parent slabclass */
    struct item *it;

    p = &slabclass[id];

    if (p->nfree_itemq == 0) {
        return NULL;
    }

    it = TAILQ_FIRST(&p->free_itemq);

    ASSERT(it->magic == ITEM_MAGIC);
    ASSERT((it->flags & ITEM_SLABBED) != 0);
    ASSERT((it->flags & ITEM_LINKED) == 0);

    it->flags &= ~ITEM_SLABBED;

    ASSERT(p->nfree_itemq > 0);
    p->nfree_itemq--;
    TAILQ_REMOVE(&p->free_itemq, it, i_tqe);
    stats_slab_decr(id, item_free);

    log_debug(LOG_VERB, "get free q it '%.*s' at offset %"PRIu32" with id "
              "%"PRIu8"", it->nkey, item_key(it), it->offset, it->id);

    return it;
}

/*
 * Get an item from the slab with a given id. We get an item either from:
 * 1. item free Q of given slab with id. or,
 * 2. current slab.
 * If the current slab is empty, we get a new slab from the slab allocator
 * and return the next item from this new slab.
 */
static struct item *
_slab_get_item(uint8_t id, bool evict)
{
    struct slabclass *p;
    struct item *it;

    p = &slabclass[id];

    it = slab_get_item_from_freeq(id);
    if (it != NULL) {
        return it;
    }

    if (p->free_item == NULL && (slab_get(id, evict) != MC_OK)) {
        return NULL;
    }

    /* return item from current slab */
    it = p->free_item;
    if (--p->nfree_item != 0) {
        p->free_item = (struct item *)(((uint8_t *)p->free_item) + p->size);
    } else {
        p->free_item = NULL;
    }

    log_debug(LOG_VERB, "get new it at offset %"PRIu32" with id %"PRIu8"",
              it->offset, it->id);

    return it;
}

struct item *
slab_get_item(uint8_t id, bool evict)
{
    struct item *it;

    ASSERT(id >= SLABCLASS_MIN_ID && id <= slabclass_max_id);

    pthread_mutex_lock(&slab_lock);
    it = _slab_get_item(id, evict);
    pthread_mutex_unlock(&slab_lock);

    return it;
}

/*
 * Put an item back into the slab by inserting into the item free Q.
 */
static void
slab_put_item_into_freeq(struct item *it)
{
    uint8_t id = it->id;
    struct slabclass *p = &slabclass[id];

    ASSERT(id >= SLABCLASS_MIN_ID && id <= slabclass_max_id);
    ASSERT(item_2_slab(it)->id == id);
    ASSERT((it->flags & (ITEM_LINKED | ITEM_SLABBED)) == 0);
    ASSERT(it->refcount == 0);
    ASSERT(it->offset != 0);

    log_debug(LOG_VERB, "put free q it '%.*s' at offset %"PRIu32" with id "
              "%"PRIu8"", it->nkey, item_key(it), it->offset, it->id);

    it->flags |= ITEM_SLABBED;

    p->nfree_itemq++;
    TAILQ_INSERT_HEAD(&p->free_itemq, it, i_tqe);

    stats_slab_incr(id, item_free);
    stats_slab_incr(id, item_remove);
}

/*
 * Put an item back into the slab
 */
static void
_slab_put_item(struct item *it)
{
    slab_put_item_into_freeq(it);
}

void
slab_put_item(struct item *it)
{
    pthread_mutex_lock(&slab_lock);
    _slab_put_item(it);
    pthread_mutex_unlock(&slab_lock);
}
