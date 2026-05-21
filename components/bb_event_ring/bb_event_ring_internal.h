#pragma once
// Internal hooks for bb_event_ring. Used by tests (failing-alloc injection)
// and by the ESP-IDF platform component (SPIRAM-preferred allocator override).
#include <stddef.h>

typedef void *(*bb_event_ring_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_event_ring_free_fn)(void *p);

void bb_event_ring_set_allocator(bb_event_ring_calloc_fn c, bb_event_ring_free_fn f);
void bb_event_ring_reset_allocator(void);
