#pragma once

// bb_mem — SPIRAM-preferred allocation helpers.
//
// On ESP-IDF these try the SPIRAM/8-bit heap first and fall back to the
// default (internal) heap, keeping large/long-lived buffers out of the scarce
// internal-RAM budget shared with TLS handshakes and real-time stacks; on
// boards without PSRAM the fallback preserves the original behaviour. On host
// they are plain malloc/calloc/free.
//
// Allocations returned by bb_malloc_prefer_spiram / bb_calloc_prefer_spiram
// MUST be released with bb_mem_free (not libc free), since the ESP-IDF backend
// allocates via heap_caps and frees via heap_caps_free.
//
// STATS COVERAGE CAVEAT: only allocations through bb_malloc_prefer_spiram,
// bb_calloc_prefer_spiram, and bb_mem_free are counted. lwIP, mbedTLS,
// FreeRTOS, esp-mqtt, and direct malloc calls bypass this accounting.
// outstanding_bytes is therefore a LOWER BOUND on heap usage attributed to
// consumers of these facades.

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Kconfig bridge for BB_MEM_STATS_ENABLE
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#  include "sdkconfig.h"
#  ifdef CONFIG_BB_MEM_STATS_ENABLE
#    define BB_MEM_STATS_ENABLE CONFIG_BB_MEM_STATS_ENABLE
#  endif
#endif
#ifndef BB_MEM_STATS_ENABLE
#define BB_MEM_STATS_ENABLE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Allocation stats snapshot
// ---------------------------------------------------------------------------

/**
 * Snapshot of bb_mem allocation accounting.
 *
 * When BB_MEM_STATS_ENABLE is 0, bb_mem_get_stats() returns a zero-filled
 * struct and all counters compile out at zero cost.
 */
typedef struct {
    size_t   outstanding_bytes;    // bytes currently allocated via bb_mem facades
    size_t   peak_outstanding;     // highest outstanding_bytes value ever observed
    uint32_t alloc_count;          // total bb_malloc/bb_calloc calls (incl. failures)
    uint32_t free_count;           // total non-NULL bb_mem_free calls
    uint32_t alloc_fail;           // calls where the allocator returned NULL
    uint32_t spiram_alloc_count;   // allocations that landed in SPIRAM
    size_t   spiram_alloc_bytes;   // bytes allocated in SPIRAM
    uint32_t internal_alloc_count; // allocations that landed in internal RAM
    size_t   internal_alloc_bytes; // bytes allocated in internal RAM
} bb_mem_stats_t;

/**
 * Fill *out with an atomic snapshot of the current allocation counters.
 * Returns a zero-filled struct when BB_MEM_STATS_ENABLE is 0.
 * Thread-safe: each counter read is individually atomic (relaxed order).
 */
void bb_mem_get_stats(bb_mem_stats_t *out);

/**
 * Reset all allocation counters to zero (test isolation).
 * No-op when BB_MEM_STATS_ENABLE is 0.
 */
void bb_mem_reset_stats(void);

// ---------------------------------------------------------------------------
// Allocation helpers
// ---------------------------------------------------------------------------

// Allocate `size` bytes, preferring SPIRAM with fallback to the default heap.
// Returns NULL on failure. Contents are uninitialised (malloc semantics).
void *bb_malloc_prefer_spiram(size_t size);

// Allocate `n * size` zero-initialised bytes, preferring SPIRAM with fallback
// to the default heap. Returns NULL on failure.
void *bb_calloc_prefer_spiram(size_t n, size_t size);

// Free a pointer returned by the bb_*_prefer_spiram helpers. NULL is a no-op.
void bb_mem_free(void *p);

#ifdef __cplusplus
}
#endif
