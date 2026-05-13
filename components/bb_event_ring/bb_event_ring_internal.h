#pragma once
#ifdef BB_EVENT_TESTING

#include <stddef.h>

typedef void *(*bb_event_ring_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_event_ring_free_fn)(void *p);

void bb_event_ring_set_allocator(bb_event_ring_calloc_fn c, bb_event_ring_free_fn f);
void bb_event_ring_reset_allocator(void);
void bb_event_reset_for_test(void);
void bb_event_reset_pool_for_test(void);
void bb_event_port_set_malloc(void *(*m)(size_t));
void bb_event_port_reset_for_test(void);

#endif
