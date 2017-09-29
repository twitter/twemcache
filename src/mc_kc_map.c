#include <mc_kc_map.h>

#include <string.h>

static struct kc_map_entry *table;
static size_t table_size;
static size_t table_nkey;

static void
kc_map_entry_reset(struct kc_map_entry *kcme)
{
    kcme->klen = 0;
    kcme->count = 0;
}

rstatus_t
kc_map_init(size_t size)
{
    int i;

    /* allocate 2x the number of entries expected */
    table_size = 2 * size;
    table = mc_alloc(sizeof(*table) * table_size);

    if (table == NULL) {
        log_error("Could not allocate counter table for hotkey - OOM");
        return MC_ENOMEM;
    }

    for (i = 0; i < table_size; ++i) {
        kc_map_entry_reset(table + i);
    }

    table_nkey = 0;

    return MC_OK;
}

void
kc_map_deinit(void)
{
    mc_free(table);
    table_size = 0;
    table_nkey = 0;
}

/* return true if empty entry or if key, klen match kcme */
static inline bool
kc_map_match(const struct kc_map_entry *kcme, const char *key, size_t klen)
{
    return kcme->count == 0 || (kcme->klen == klen && strncmp(kcme->key, key, klen) == 0);
}

struct kc_map_entry *
kc_map_incr(const char *key, size_t klen)
{
    size_t entry;

    ASSERT(table_nkey < table_size);

    /* hash, then iterate until we find either a match or empty entry */
    for (entry = hash(key, klen, 0) % table_size;
         !kc_map_match(table + entry, key, klen);
         entry = (entry + 1) % table_size);

    if ((table + entry)->count == 0) {
        memcpy((table + entry)->key, key, klen);
        (table + entry)->klen = klen;
        (table + entry)->count = 1;
    } else {
        ++((table + entry)->count);
    }

    return table + entry;
}

void
kc_map_decr(struct kc_map_entry *kcme)
{
    if (kcme->count == 1) {
        kc_map_entry_reset(kcme);
    } else {
        --(kcme->count);
    }
}
