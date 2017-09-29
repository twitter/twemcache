#ifndef _MC_KC_MAP_H_
#define _MC_KC_MAP_H_

#include <mc_core.h>

struct kc_map_entry {
    char                       key[MAX_KEY_LEN];
    size_t                     klen;
    size_t                     count;
};

rstatus_t kc_map_init(size_t size);
void kc_map_deinit(void);

struct kc_map_entry *kc_map_incr(const char *key, size_t klen);
void kc_map_decr(struct kc_map_entry *kcme);

#endif
