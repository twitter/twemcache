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

#include <mc_core.h>

#define HASHSIZE(_n) (1UL << (_n))
#define HASHMASK(_n) (HASHSIZE(_n) - 1)

#define HASH_DEFAULT_MOVE_SIZE  1
#define HASH_DEFAULT_POWER      16

extern struct settings settings;
extern pthread_mutex_t cache_lock;

/*
 * We always look for items in the primary_hashtable expect when we are
 * expanding. During expansion (expanding = 1), we migrate key-values at
 * bucket granularity - nhash_move_size from old_hashtable to primary_hashtable.
 * The expand_bucket tells us how far we have gotton and it takes
 * values in the range [0, HASHSIZE(hash_power - 1) - 1]
 */
static struct item_slh *primary_hashtable;  /* primary (main) hash table */
static struct item_slh *old_hashtable;      /* secondary (old) hash table */
static uint32_t nhash_item;                 /* # items in hash table */
static uint32_t hash_power;                 /* # buckets = 2^hash_power */

static int expanding;                       /* expanding? */
static uint32_t nhash_move_size;            /* # hash buckets to move during expansion */
static uint32_t expand_bucket;              /* last expanded bucket */

static pthread_cond_t maintenance_cond;     /* maintenance thread condvar */
static pthread_t maintenance_tid;           /* maintenance thread id */
static volatile int run_maintenance_thread; /* run maintenance thread? */

static void *
assoc_maintenance_thread(void *arg)
{
    uint32_t i, hv;
    struct item_slh *old_bucket, *new_bucket;
    struct item *it, *next;

    while (run_maintenance_thread) {
        /*
         * Lock the cache, and bulk move multiple buckets to the new
         * hash table
         */
        pthread_mutex_lock(&cache_lock);

        for (i = 0; i < nhash_move_size && expanding == 1; i++) {

            old_bucket = &old_hashtable[expand_bucket];

            SLIST_FOREACH_SAFE(it, old_bucket, h_sle, next) {
                hv = hash(item_key(it), it->nkey, 0);
                new_bucket = &primary_hashtable[hv & HASHMASK(hash_power)];
                SLIST_REMOVE(old_bucket, it, item, h_sle);
                SLIST_INSERT_HEAD(new_bucket, it, h_sle);
            }

            expand_bucket++;
            if (expand_bucket == HASHSIZE(hash_power - 1)) {
                expanding = 0;
                mc_free(old_hashtable);
            }
        }

        if (expanding == 0) {
            /* we are done expanding, just wait for the next invocation */
            pthread_cond_wait(&maintenance_cond, &cache_lock);
        }

        pthread_mutex_unlock(&cache_lock);
    }

    return NULL;
}

static rstatus_t
assoc_start_maintenance_thread(void)
{
    err_t err;

    err = pthread_create(&maintenance_tid, NULL, assoc_maintenance_thread,
                         NULL);
    if (err != 0) {
        log_error("pthread create failed: %s", strerror(err));
        return MC_ERROR;
    }

    return MC_OK;
}

static void
assoc_stop_maintenance_thread(void)
{
    pthread_mutex_lock(&cache_lock);
    run_maintenance_thread = 0;
    pthread_cond_signal(&maintenance_cond);
    pthread_mutex_unlock(&cache_lock);

    /* wait for the maintenance thread to stop */
    pthread_join(maintenance_tid, NULL);
}

static struct item_slh *
assoc_create_table(uint32_t table_sz)
{
    struct item_slh *table;
    uint32_t i;

    table = mc_alloc(sizeof(*table) * table_sz);
    if (table == NULL) {
        return NULL;
    }

    for (i = 0; i < table_sz; i++) {
        SLIST_INIT(&table[i]);
    }

    return table;
}

static struct item_slh *
assoc_get_bucket(const char *key, size_t nkey)
{
    struct item_slh *bucket;
    uint32_t hv, oldbucket, curbucket;

    hv = hash(key, nkey, 0);
    oldbucket = hv & HASHMASK(hash_power - 1);
    curbucket = hv & HASHMASK(hash_power);

    if ((expanding == 1) && oldbucket >= expand_bucket) {
        bucket = &old_hashtable[oldbucket];
    } else {
        bucket = &primary_hashtable[curbucket];
    }

    return bucket;
}

rstatus_t
assoc_init(void)
{
    rstatus_t status;
    uint32_t hashtable_sz;

    primary_hashtable = NULL;
    hash_power = settings.hash_power > 0 ? settings.hash_power : HASH_DEFAULT_POWER;

    old_hashtable = NULL;
    nhash_move_size = HASH_DEFAULT_MOVE_SIZE;
    nhash_item = 0;
    expanding = 0;
    expand_bucket = 0;

    hashtable_sz = HASHSIZE(hash_power);

    primary_hashtable = assoc_create_table(hashtable_sz);
    if (primary_hashtable == NULL) {
        return MC_ENOMEM;
    }

    pthread_cond_init(&maintenance_cond, NULL);
    run_maintenance_thread = 1;

    status = assoc_start_maintenance_thread();
    if (status != MC_OK) {
        return status;
    }

    return MC_OK;
}

void
assoc_deinit(void)
{
    assoc_stop_maintenance_thread();
}

struct item *
assoc_find(const char *key, size_t nkey)
{
    struct item_slh *bucket;
    struct item *it;
    uint32_t depth;

    ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
    ASSERT(key != NULL && nkey != 0);

    bucket = assoc_get_bucket(key, nkey);

    for (depth = 0, it = SLIST_FIRST(bucket); it != NULL;
         depth++, it = SLIST_NEXT(it, h_sle)) {
        if ((nkey == it->nkey) && (memcmp(key, item_key(it), nkey) == 0)) {
            break;
        }
    }

    return it;
}

static bool
assoc_expand_needed(void)
{
    return ((settings.hash_power == 0) && (expanding == 0) &&
            (nhash_item > (HASHSIZE(hash_power) * 3 / 2)));
}

/*
 * Expand the hashtable to the next power of 2. On failure, continue using
 * the old hashtable
 */
static void
assoc_expand(void)
{
    uint32_t hashtable_sz = HASHSIZE(hash_power + 1);

    old_hashtable = primary_hashtable;
    primary_hashtable = assoc_create_table(hashtable_sz);
    if (primary_hashtable == NULL) {
        primary_hashtable = old_hashtable;
        return;
    }

    log_debug(LOG_INFO, "expanding hash table with %"PRIu32" items to "
              "%"PRIu32" buckets of size %"PRIu32" bytes", nhash_item,
              hashtable_sz, sizeof(struct item_slh) * hashtable_sz);

    hash_power++;
    expanding = 1;
    expand_bucket = 0;

    pthread_cond_signal(&maintenance_cond);
}

void
assoc_insert(struct item *it)
{
    struct item_slh *bucket;

    ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
    ASSERT(assoc_find(item_key(it), it->nkey) == NULL);

    bucket = assoc_get_bucket(item_key(it), it->nkey);
    SLIST_INSERT_HEAD(bucket, it, h_sle);
    nhash_item++;

    if (assoc_expand_needed()) {
        assoc_expand();
    }
}

void
assoc_delete(const char *key, size_t nkey)
{
    struct item_slh *bucket;
    struct item *it, *prev;

    ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
    ASSERT(assoc_find(key, nkey) != NULL);

    bucket = assoc_get_bucket(key, nkey);

    for (prev = NULL, it = SLIST_FIRST(bucket); it != NULL;
         prev = it, it = SLIST_NEXT(it, h_sle)) {
        if ((nkey == it->nkey) && (memcmp(key, item_key(it), nkey) == 0)) {
            break;
        }
    }

    if (prev == NULL) {
        SLIST_REMOVE_HEAD(bucket, h_sle);
    } else {
        SLIST_REMOVE_AFTER(prev, h_sle);
    }

    nhash_item--;
}
