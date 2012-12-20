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

#ifndef _MC_ITEMS_H_
#define _MC_ITEMS_H_

typedef enum item_flags {
    ITEM_LINKED  = 1,  /* item in lru q and hash */
    ITEM_CAS     = 2,  /* item has cas */
    ITEM_SLABBED = 4,  /* item in free q */
    ITEM_RALIGN  = 8,  /* item data (payload) is right-aligned */
} item_flags_t;

typedef enum item_store_result {
    NOT_STORED,
    STORED,
    EXISTS,
    NOT_FOUND
} item_store_result_t;

typedef enum item_delta_result {
    DELTA_OK,
    DELTA_NON_NUMERIC,
    DELTA_EOM,
    DELTA_NOT_FOUND,
} item_delta_result_t;

/*
 * Every item chunk in the twemcache starts with an header (struct item)
 * followed by item data. An item is essentially a chunk of memory
 * carved out of a slab. Every item is owned by its parent slab
 *
 * Items are either linked or unlinked. When item is first allocated and
 * has no data, it is unlinked. When data is copied into an item, it is
 * linked into hash and lru q (ITEM_LINKED). When item is deleted either
 * explicitly or due to flush or expiry, it is moved in the free q
 * (ITEM_SLABBED). The flags ITEM_LINKED and ITEM_SLABBED are mutually
 * exclusive and when an item is unlinked it has neither of these flags
 *
 *   <-----------------------item size------------------>
 *   +---------------+----------------------------------+
 *   |               |                                  |
 *   |  item header  |          item payload            |
 *   | (struct item) |         ...      ...             |
 *   +---------------+-------+-------+------------------+
 *   ^               ^       ^       ^
 *   |               |       |       |
 *   |               |       |       |
 *   |               |       |       |
 *   |               |       |       \
 *   |               |       |       item_data()
 *   |               |       \
 *   \               |       item_key()
 *   item            \
 *                   item->end, (if enabled) item_cas()
 *
 * item->end is followed by:
 * - 8-byte cas, if ITEM_CAS flag is set
 * - key with terminating '\0', length = item->nkey + 1
 * - data with no terminating '\0'
 */
struct item {
    uint32_t          magic;      /* item magic (const) */
    TAILQ_ENTRY(item) i_tqe;      /* link in lru q or free q */
    SLIST_ENTRY(item) h_sle;      /* link in hash */
    rel_time_t        atime;      /* last access time in secs */
    rel_time_t        exptime;    /* expiry time in secs */
    uint32_t          nbyte;      /* date size */
    uint32_t          offset;     /* offset of item in slab */
    uint32_t          dataflags;  /* data flags opaque to the server */
    uint16_t          refcount;   /* # concurrent users of item */
    uint8_t           flags;      /* item flags */
    uint8_t           id;         /* slab class id */
    uint8_t           nkey;       /* key length */
    char              end[1];     /* item data */
};

SLIST_HEAD(item_slh, item);

TAILQ_HEAD(item_tqh, item);

#define ITEM_MAGIC      0xfeedface
#define ITEM_HDR_SIZE   offsetof(struct item, end)

/*
 * An item chunk is the portion of the memory carved out from the slab
 * for an item. An item chunk contains the item header followed by item
 * data.
 *
 * The smallest item data is actually a single byte key with a zero byte value
 * which internally is of sizeof("k"), as key is stored with terminating '\0'.
 * If cas is enabled, then item payload should have another 8-byte for cas.
 *
 * The largest item data is actually the room left in the slab_size()
 * slab, after the item header has been factored out
 */
#define ITEM_MIN_PAYLOAD_SIZE  (sizeof("k") + sizeof(uint64_t))
#define ITEM_MIN_CHUNK_SIZE \
    MC_ALIGN(ITEM_HDR_SIZE + ITEM_MIN_PAYLOAD_SIZE, MC_ALIGNMENT)

#define ITEM_PAYLOAD_SIZE      32
#define ITEM_CHUNK_SIZE     \
    MC_ALIGN(ITEM_HDR_SIZE + ITEM_PAYLOAD_SIZE, MC_ALIGNMENT)


#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 2
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

static inline bool
item_has_cas(struct item *it) {
    return (it->flags & ITEM_CAS);
}

static inline bool
item_is_linked(struct item *it) {
    return (it->flags & ITEM_LINKED);
}

static inline bool
item_is_slabbed(struct item *it) {
    return (it->flags & ITEM_SLABBED);
}

static inline bool
item_is_raligned(struct item *it) {
    return (it->flags & ITEM_RALIGN);
}

static inline uint64_t
item_cas(struct item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);

    if (item_has_cas(it)) {
        return *((uint64_t *)it->end);
    }

    return 0;
}

static inline void
item_set_cas(struct item *it, uint64_t cas)
{
    ASSERT(it->magic == ITEM_MAGIC);

    if (item_has_cas(it)) {
        *((uint64_t *)it->end) = cas;
    }
}
#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 2
#pragma GCC diagnostic pop
#endif

static inline char *
item_key(struct item *it)
{
    char *key;

    ASSERT(it->magic == ITEM_MAGIC);

    key = it->end;
    if (item_has_cas(it)) {
        key += sizeof(uint64_t);
    }

    return key;
}

static inline size_t
item_ntotal(uint8_t nkey, uint32_t nbyte, bool use_cas)
{
    size_t ntotal;

    ntotal = use_cas ? sizeof(uint64_t) : 0;
    ntotal += ITEM_HDR_SIZE + nkey + 1 + nbyte + CRLF_LEN;

    return ntotal;
}

static inline size_t
item_size(struct item *it)
{

    ASSERT(it->magic == ITEM_MAGIC);

    return item_ntotal(it->nkey, it->nbyte, item_has_cas(it));
}

void item_init(void);
void item_deinit(void);

char * item_data(struct item *it);
struct slab *item_2_slab(struct item *it);

void item_hdr_init(struct item *it, uint32_t offset, uint8_t id);

uint8_t item_slabid(uint8_t nkey, uint32_t nbyte);
struct item *item_alloc(uint8_t id, char *key, uint8_t nkey, uint32_t dataflags, rel_time_t exptime, uint32_t nbyte);

void item_reuse(struct item *it);

void item_delete(struct item *it);

void item_remove(struct item *it);
void item_touch(struct item *it);
char *item_cache_dump(uint8_t id, uint32_t limit, uint32_t *bytes);

struct item *item_get(const char *key, size_t nkey);
void item_flush_expired(void);

item_store_result_t item_store(struct item *it, req_type_t type, struct conn *c);
item_delta_result_t item_add_delta(struct conn *c, char *key, size_t nkey, int incr, int64_t delta, char *buf);

#endif
