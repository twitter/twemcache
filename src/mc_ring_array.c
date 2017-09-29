#include <mc_ring_array.h>

#include <string.h>

struct ring_array {
    size_t      elem_size;         /* element size */
    uint32_t    cap;               /* total capacity (# items stored + 1) */
    uint32_t    rpos;              /* read offset */
    uint32_t    wpos;              /* write offset */
    union {
        size_t  pad;               /* using a size_t member to force alignment at
                                      native word boundary */
        uint8_t data[1];           /* beginning of array */
    };
};

#define RING_ARRAY_HDR_SIZE offsetof(struct ring_array, data)

/**
 * The total number of slots allocated is (cap + 1)
 *
 * Each ring array should have exactly one reader and exactly one writer, as
 * far as threads are concerned (which can be the same). This allows the use of
 * atomic instructions to replace locks.
 *
 * We use an extra slot to differentiate full from empty.
 *
 * 1) If rpos == wpos, the buffer is empty.
 *
 * 2) If rpos is behind wpos (see below):
 *     # of occupied slots: wpos - rpos
 *     # of vacant slots: rpos + cap - wpos + 1
 *     # of writable slots: rpos + cap - wpos
 *     full if: rpos == 0, wpos == cap
 *
 *       0                       cap
 *       |                       |
 *       v                       v
 *      +-+-+-+---------------+-+-+
 * data | | | |      ...      | | |
 *      +-+-+-+---------------+-+-+
 *           ^             ^
 *           |             |
 *           rpos          wpos
 *
 * 3) If rpos is ahead of wpos (see below):
 *     # of occupied slots: wpos + cap - rpos + 1
 *     # of vacant slots: rpos - wpos
 *     # of writable slots: rpos - wpos - 1
 *     full if: rpos == wpos + 1
 *
 *       0                       cap
 *       |                       |
 *       v                       v
 *      +-+-+-+---------------+-+-+
 * data | | | |      ...      | | |
 *      +-+-+-+---------------+-+-+
 *           ^             ^
 *           |             |
 *           wpos          rpos
 *
 */

static inline uint32_t
ring_array_nelem(uint32_t rpos, uint32_t wpos, uint32_t cap)
{
    if (rpos <= wpos) { /* condition 1, 2) */
        return wpos - rpos;
    } else {            /* condition 3 */
        return wpos + (cap - rpos + 1);
    }
}

static inline bool
ring_array_empty(uint32_t rpos, uint32_t wpos)
{
    return rpos == wpos;
}

static inline bool
ring_array_full(uint32_t rpos, uint32_t wpos, uint32_t cap)
{
    return ring_array_nelem(rpos, wpos, cap) == cap;
}

rstatus_t
ring_array_push(const void *elem, struct ring_array *arr)
{
    /**
     * Take snapshot of rpos, since another thread might be popping. Note: other
     * members of arr do not need to be saved because we assume the other thread
     * only pops and does not push; in other words, only one thread updates
     * either rpos or wpos.
     */
    uint32_t new_wpos;
    uint32_t rpos = __atomic_load_n(&(arr->rpos), __ATOMIC_RELAXED);

    if (ring_array_full(rpos, arr->wpos, arr->cap)) {
        log_debug(LOG_DEBUG, "Could not push to ring array %p; array is full", arr);
        return MC_ERROR;
    }

    memcpy(arr->data + (arr->elem_size * arr->wpos), elem, arr->elem_size);

    /* update wpos atomically */
    new_wpos = (arr->wpos + 1) % (arr->cap + 1);
    __atomic_store_n(&(arr->wpos), new_wpos, __ATOMIC_RELAXED);

    return MC_OK;
}

rstatus_t
ring_array_pop(void *elem, struct ring_array *arr)
{
    /* take snapshot of wpos, since another thread might be pushing */
    uint32_t new_rpos;
    uint32_t wpos = __atomic_load_n(&(arr->wpos), __ATOMIC_RELAXED);

    if (ring_array_empty(arr->rpos, wpos)) {
        log_debug(LOG_DEBUG, "Could not pop from ring array %p; array is empty", arr);
        return MC_ERROR;
    }

    if (elem != NULL) {
        memcpy(elem, arr->data + (arr->elem_size * arr->rpos), arr->elem_size);
    }

    /* update rpos atomically */
    new_rpos = (arr->rpos + 1) % (arr->cap + 1);
    __atomic_store_n(&(arr->rpos), new_rpos, __ATOMIC_RELAXED);

    return MC_OK;
}

struct ring_array *
ring_array_create(size_t elem_size, uint32_t cap)
{
    struct ring_array *arr;

    arr = mc_alloc(RING_ARRAY_HDR_SIZE + elem_size * (cap + 1));

    if (arr == NULL) {
        log_error("Could not allocate memory for ring array cap %u "
                  "elem_size %u", cap, elem_size);
        return NULL;
    }

    arr->elem_size = elem_size;
    arr->cap = cap;
    arr->rpos = arr->wpos = 0;
    return arr;
}

void
ring_array_destroy(struct ring_array **arr)
{
    mc_free(*arr);
    *arr = NULL;
}
