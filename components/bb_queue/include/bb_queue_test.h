#pragma once
// Test-only hooks for bb_queue. Gated on BB_QUEUE_TESTING so production builds
// never compile against them. Tests include this header instead of reaching
// into bb_queue.c internals.
#ifdef BB_QUEUE_TESTING

#include "bb_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// bb_queue_test_force_bytes_used — directly overwrite the internal bytes_used
// accounting field, bypassing the normal push/pop increment/decrement path.
// Used to simulate accounting drift (e.g. a value smaller than a subsequent
// pop's entry length) so tests can exercise the underflow-clamp branch in
// bb_queue_pop_oldest()/bb_queue_push() without relying on a real bug to exist.
void bb_queue_test_force_bytes_used(bb_queue_t r, size_t value);

#ifdef __cplusplus
}
#endif

#endif
