#include <mc_core.h>

static size_t key_window_size;
static size_t key_window_max;

struct ring_array *queue;

struct kw_entry {
    struct kc_map_entry *kcme;
    uint64_t timestamp_us;      /* timestamp in usec */
};

size_t
key_window_push(const char *key, size_t klen, uint64_t time)
{
    rstatus_t status;
    struct kw_entry kwe;

    ASSERT(klen <= MAX_KEY_LEN);

    kwe.timestamp_us = time;
    kwe.kcme = kc_map_incr(key, klen);
    status = ring_array_push(&kwe, queue);
    ++key_window_size;

    ASSERT(status == MC_OK);
    (void)status;

    return kwe.kcme->count;
}

uint64_t
key_window_pop(void)
{
    rstatus_t status;
    struct kw_entry kwe;

    status = ring_array_pop(&kwe, queue);
    --key_window_size;

    ASSERT(status == MC_OK);
    (void)status;

    kc_map_decr(kwe.kcme);
    return kwe.timestamp_us;
}

bool
key_window_full(void)
{
    return key_window_size == key_window_max;
}

rstatus_t
key_window_init(size_t size)
{
    if ((queue = ring_array_create(sizeof(struct kw_entry), size)) == NULL) {
        return MC_ENOMEM;
    }

    key_window_size = 0;
    key_window_max = size;
    return MC_OK;
}

void
key_window_deinit(void)
{
    ring_array_destroy(&queue);
    key_window_size = 0;
    key_window_max = 0;
}
