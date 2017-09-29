#ifndef _MC_RING_ARRAY_H_
#define _MC_RING_ARRAY_H_

#include <mc_core.h>

struct ring_array;

/* push an element into the array */
rstatus_t ring_array_push(const void *elem, struct ring_array *arr);

/* pop an element from the array */
rstatus_t ring_array_pop(void *elem, struct ring_array *arr);

/* creation/destruction */
struct ring_array *ring_array_create(size_t elem_size, uint32_t cap);
void ring_array_destroy(struct ring_array **arr);

#endif
