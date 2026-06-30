#pragma once

// bb_mem_test — host-only test seam for injecting allocation failures.
//
// Include this header in host unit tests that need to exercise error branches
// where bb_malloc_prefer_spiram / bb_calloc_prefer_spiram return NULL.
//
// Guard: only compiled when BB_MEM_TESTING is defined (add -DBB_MEM_TESTING
// to platformio.ini build_flags for the native test env).

#ifdef BB_MEM_TESTING
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install a malloc override for testing fail paths.
 *
 * When fn is non-NULL it is called instead of the underlying allocator for
 * both bb_malloc_prefer_spiram and bb_calloc_prefer_spiram.  Pass NULL to
 * restore normal allocation.
 *
 * Not thread-safe — call only from a single test thread while no concurrent
 * allocations are in flight.
 */
void bb_mem_set_alloc_hook(void *(*fn)(size_t));

#ifdef __cplusplus
}
#endif
#endif // BB_MEM_TESTING
