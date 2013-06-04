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

#ifndef _MC_SLABS_H_
#define _MC_SLABS_H_

#include <mc_items.h>

/*
 * Every slab (struct slab) in the twemcache starts with a slab header
 * followed by slab data. The slab data is essentially a collection of
 * contiguous, equal sized items (struct item)
 *
 * An item is owned by a slab and a slab is owned by a slabclass
 *
 *   <------------------------ slab_size ------------------------->
 *   +---------------+--------------------------------------------+
 *   |  slab header  |              slab data                     |
 *   | (struct slab) |      (contiguous equal sized items)        |
 *   +---------------+--------------------------------------------+
 *   ^               ^
 *   |               |
 *   \               |
 *   slab            \
 *                   slab->data
 *
 * Note: keep struct slab 8-byte aligned so that item chunks always start on
 *       8-byte aligned boundary.
 */
struct slab {
#if MC_ASSERT_PANIC == 1 || MC_ASSERT_LOG == 1
    uint32_t          magic;    /* slab magic (const) */
#endif
    uint8_t           id;       /* slabclass id */
    uint8_t           unused;   /* unused */
    uint16_t          refcount; /* # concurrent users */
    TAILQ_ENTRY(slab) s_tqe;    /* link in slab lruq */
    rel_time_t        utime;    /* last update time in secs */
    uint32_t          padding;  /* unused */
    uint8_t           data[1];  /* opaque data */
};

TAILQ_HEAD(slab_tqh, slab);

#define SLAB_MAGIC      0xdeadbeef
#define SLAB_HDR_SIZE   offsetof(struct slab, data)
#define SLAB_MIN_SIZE   ((size_t) 512)
#define SLAB_MAX_SIZE   ((size_t) (128 * MB))
#define SLAB_SIZE       MB

/*
 * Every class (struct slabclass) is a collection of slabs that can serve
 * items of a given maximum size. Every slab in twemcache is identified by a
 * unique unsigned 8-bit id, which also identifies its owner slabclass
 *
 * Slabs that belong to a given class are reachable through slabq. Slabs
 * across all classes are reachable through the slabtable and slab lruq.
 *
 * We use free_item as a marker for the next available, unallocated item
 * in the current slab. Items that are available for reuse (i.e. allocated
 * and then freed) are kept track by free_itemq
 *
 * slabclass[]:
 *
 *  +-------------+
 *  |             |
 *  |             |
 *  |             |
 *  |   class 0   |
 *  |             |
 *  |             |
 *  |             |
 *  +-------------+
 *  |             |  ----------------------------------------------------------+
 *  |             | /                                              (last slab) |
 *  |             |/    +---------------+-------------------+    +-------------v-+-------------------+
 *  |             |     |               |                   |    |               |                   |
 *  |   class 1   |     |  slab header  |     slab data     |    |  slab header  |     slab data     |
 *  |             |     |               |                   |    |               |                   |--+
 *  |             |\    +---------------+-------------------+    +---------------+-------------------+  |
 *  |             | \                                                                                   //
 *  |             |  ----> (freeq)
 *  +-------------+
 *  |             |  -----------------+
 *  |             | /     (last slab) |
 *  |             |/    +-------------v-+-------------------+
 *  |             |     |               |                   |
 *  |   class 2   |     |  slab header  |     slab data     |
 *  |             |     |               |                   |--+
 *  |             |\    +---------------+-------------------+  |
 *  |             | \                                          //
 *  |             |  ----> (freeq)
 *  +-------------+
 *  |             |
 *  |             |
 *  .             .
 *  .    ....     .
 *  .             .
 *  |             |
 *  |             |
 *  +-------------+
 *            |
 *            |
 *            //
 */
struct slabclass {
    uint32_t        nitem;       /* # item per slab (const) */
    size_t          size;        /* item size (const) */

    uint32_t        nfree_itemq; /* # free item q */
    struct item_tqh free_itemq;  /* free item q */

    uint32_t        nfree_item;  /* # free item (in current slab) */
    struct item     *free_item;  /* next free item (in current slab) */
};

/*
 * Slabclass id is an unsigned byte. So, maximum number of slab classes
 * cannot exceeded 256
 *
 * We use id = 255 as an invalid id and id = 0 for aggregation. This means
 * that we can have at most 254 usable slab classes
 */
#define SLABCLASS_MIN_ID        1
#define SLABCLASS_MAX_ID        (UCHAR_MAX - 1)
#define SLABCLASS_INVALID_ID    UCHAR_MAX
#define SLABCLASS_MAX_IDS       UCHAR_MAX

size_t slab_size(void);
void slab_print(void);
void slab_acquire_refcount(struct slab *slab);
void slab_release_refcount(struct slab *slab);
size_t slab_item_size(uint8_t id);
uint8_t slab_id(size_t size);

rstatus_t slab_init(void);
void slab_deinit(void);

struct item *slab_get_item(uint8_t id);
void slab_put_item(struct item *it);
void slab_lruq_touch(struct slab *slab, bool allocated);

#endif
