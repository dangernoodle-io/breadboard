#pragma once
// Internal hooks for bb_event_ring.
//
// After the bb_queue consolidation, bb_event_ring stores all entry data via a
// bb_queue_t internally.  The allocator hook below is a shim that forwards to
// bb_queue_set_allocator / bb_queue_reset_allocator — storage for the ring
// entries lands wherever bb_queue allocates (SPIRAM-preferred on ESP-IDF via the
// bb_queue_espidf platform component).
//
// The functions are still declared here so that existing tests and the legacy
// bb_event_ring_spiram.c shim continue to compile unmodified.
#include <stddef.h>

typedef void *(*bb_event_ring_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_event_ring_free_fn)(void *p);

// Forwards to bb_queue_set_allocator.
void bb_event_ring_set_allocator(bb_event_ring_calloc_fn c, bb_event_ring_free_fn f);
// Forwards to bb_queue_reset_allocator.
void bb_event_ring_reset_allocator(void);
