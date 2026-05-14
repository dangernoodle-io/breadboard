#pragma once
// Test-only hooks for bb_event_ring. For bb_event / port test hooks see
// components/bb_event/bb_event_test.h — they are NOT declared here.
#ifdef BB_EVENT_TESTING

#include <stddef.h>

typedef void *(*bb_event_ring_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_event_ring_free_fn)(void *p);

void bb_event_ring_set_allocator(bb_event_ring_calloc_fn c, bb_event_ring_free_fn f);
void bb_event_ring_reset_allocator(void);

#endif
