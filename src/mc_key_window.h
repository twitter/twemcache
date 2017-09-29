#ifndef _MC_KEY_WINDOW_H_
#define _MC_KEY_WINDOW_H_

#include <stdlib.h>

#include <mc_core.h>

size_t key_window_push(const char *key, size_t klen, uint64_t time);
uint64_t key_window_pop(void);
bool key_window_full(void);

rstatus_t key_window_init(size_t size);
void key_window_deinit(void);

#endif
