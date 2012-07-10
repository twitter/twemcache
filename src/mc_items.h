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
 *   <-----------------------item size--------------------------->
 *   +---------------+-------------------------------------------+
 *   |               |                                           |
 *   |  item header  |              item data                    |
 *   | (struct item) |     ...      ...                          |
 *   +---------------+---+-----+-----------+---------------------+
 *   ^               ^   ^     ^           ^
 *   |               |   |     |           |
 *   |               |   |     |           \
 *   |               |   |     |           item_data()
 *   |               |   |     \
 *   |               |   |     item_suffix()
 *   |               |   \
 *   \               |   item_key()
 *   item            \
 *                   item->end, item_cas()
 *
 * item->end is followed by:
 * - 8-byte cas, if ITEM_CAS flag is set
 * - '\0' key of length = item->nkey + 1
 * - " flags length\r\n" with no terminating '\0'
 * - data terminated with CRLF
 */
struct item {
    uint32_t          magic;      /* item magic (const) */
    TAILQ_ENTRY(item) i_tqe;      /* link in lru q or free q */
    SLIST_ENTRY(item) h_sle;      /* link in hash */
    rel_time_t        atime;      /* last access time in secs */
    rel_time_t        exptime;    /* expiry time in secs */
    uint32_t          nbyte;      /* date size */
    uint32_t          offset;     /* offset of item in slab */
    uint16_t          refcount;   /* # concurrent users of item */
    uint8_t           nsuffix;    /* length of flags-and-length string */
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
 * The smallest item data is actually a single byte key with a zero
 * byte value which is of sizeof("k 0 1\r\n1\r\n") - 1. If cas is enabled,
 * then item data should also have enough room for an 8-byte cas value.
 *
 * The largest item data is actually the room left in the slab_size()
 * slab, after the item header has been factored out
 */
#define ITEM_MIN_DATA_SIZE  (sizeof("k 0 0\r\n\r\n") - 1 + sizeof(uint64_t))
#define ITEM_MIN_CHUNK_SIZE \
    MC_ALIGN(ITEM_HDR_SIZE + ITEM_MIN_DATA_SIZE, MC_ALIGNMENT)

#define ITEM_DATA_SIZE      32
#define ITEM_CHUNK_SIZE     \
    MC_ALIGN(ITEM_HDR_SIZE + ITEM_DATA_SIZE, MC_ALIGNMENT)

#define ITEM_MIN_SUFFIX_LEN (sizeof("0 0") - 1)
#define ITEM_MAX_SUFFIX_LEN 40


#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 2
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif
static inline uint64_t
item_cas(struct item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);

    if (it->flags & ITEM_CAS) {
        return *((uint64_t *)it->end);
    }

    return 0;
}

static inline void
item_set_cas(struct item *it, uint64_t cas)
{
    ASSERT(it->magic == ITEM_MAGIC);

    if (it->flags & ITEM_CAS) {
        *((uint64_t *)it->end) = cas;
    }
}
#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 6
#pragma GCC diagnostic pop
#endif

static inline char *
item_key(struct item *it)
{
    char *key;

    ASSERT(it->magic == ITEM_MAGIC);

    key = it->end;
    if (it->flags & ITEM_CAS) {
        key += sizeof(uint64_t);
    }

    return key;
}

static inline char *
item_suffix(struct item *it)
{
    char *suffix;

    ASSERT(it->magic == ITEM_MAGIC);

    suffix = it->end + it->nkey + 1;
    if (it->flags & ITEM_CAS) {
        suffix += sizeof(uint64_t);
    }

    return suffix;
}

static inline char *
item_data(struct item *it)
{
    char *data;

    ASSERT(it->magic == ITEM_MAGIC);

    data = it->end + it->nkey + 1 + it->nsuffix;
    if (it->flags & ITEM_CAS) {
        data += sizeof(uint64_t);
    }

    return data;
}

static inline uint32_t
item_ntotal(struct item *it)
{
    uint32_t ntotal;

    ASSERT(it->magic == ITEM_MAGIC);

    ntotal = ITEM_HDR_SIZE + it->nkey + 1 + it->nsuffix + it->nbyte;
    if (it->flags & ITEM_CAS) {
        ntotal += sizeof(uint64_t);
    }

    return ntotal;
}

void item_init(void);
void item_deinit(void);

struct slab *item_2_slab(struct item *it);
bool item_size_ok(size_t nkey, int flags, int nbyte);

void item_hdr_init(struct item *it, uint32_t offset, uint8_t id);

struct item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbyte);

void item_reuse(struct item *it);

void item_link(struct item *it);
void item_unlink(struct item *it);

void item_remove(struct item *item);
void item_update(struct item *item);
char *item_cache_dump(uint8_t id, uint32_t limit, uint32_t *bytes);

struct item *item_get(const char *key, size_t nkey);
void item_flush_expired(void);

item_store_result_t item_store(struct item *item, req_type_t type, struct conn *c);
item_delta_result_t item_add_delta(struct conn *c, char *key, size_t nkey, int incr, int64_t delta, char *buf);

#endif
