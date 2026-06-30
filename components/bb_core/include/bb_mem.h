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
// STATS COVERAGE CAVEAT: only allocations through bb_mem facade functions are
// counted. lwIP, mbedTLS, FreeRTOS, esp-mqtt, and direct malloc calls bypass
// this accounting. outstanding_bytes is therefore a LOWER BOUND on heap usage
// attributed to consumers of these facades.

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Kconfig bridge for BB_MEM_STATS_ENABLE
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#  include "sdkconfig.h"
#  ifdef CONFIG_BB_MEM_STATS_ENABLE
#    undef  BB_MEM_STATS_ENABLE  /* suppress -Wmacro-redefined when build_flags and Kconfig both define this */
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
    uint32_t alloc_count;          // total bb_mem allocator/realloc calls (incl. failures)
    uint32_t free_count;           // total non-NULL bb_mem_free calls
    uint32_t alloc_fail;           // calls where the allocator returned NULL
    uint32_t spiram_alloc_count;   // allocations that landed in SPIRAM
    size_t   spiram_alloc_bytes;   // bytes currently in SPIRAM (realloc-aware: adjusts on cross-heap moves)
    uint32_t internal_alloc_count; // allocations that landed in internal RAM
    size_t   internal_alloc_bytes; // bytes currently in internal RAM (realloc-aware: adjusts on cross-heap moves)
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

// Resize a block previously allocated by any bb_mem helper, preferring SPIRAM
// with fallback to the default heap. On success returns the new pointer; on
// failure returns NULL and the original block is still valid.
//
// CAVEAT: do NOT pass pointers originally from bb_malloc_internal or
// bb_malloc_dma — crossing heap capability domains via realloc can silently
// move the block to the wrong heap (IDF crosses heaps on cap mismatch).
void *bb_realloc_prefer_spiram(void *ptr, size_t new_size);

// Free a pointer returned by any bb_mem allocator helper. NULL is a no-op.
void bb_mem_free(void *p);

// ---------------------------------------------------------------------------
// Capability-constrained helpers
// ---------------------------------------------------------------------------

// Allocate `size` bytes in internal (IRAM/DRAM) RAM only — no SPIRAM fallback.
// Use when the allocation MUST stay out of PSRAM (e.g. mbedTLS arena, control
// structures that must be visible to hardware).
// Returns NULL on failure. Contents are uninitialised.
void *bb_malloc_internal(size_t size);

// Allocate `n * size` zero-initialised bytes in internal RAM only.
// No SPIRAM fallback. Use for zero-init internal-only buffers (mbedTLS arena).
void *bb_calloc_internal(size_t n, size_t size);

// Allocate `size` bytes from the DMA-capable heap.
//
// HARD FAIL — NO FALLBACK. Returns NULL rather than fall back to a non-DMA
// heap. On classic ESP32, PSRAM is not DMA-accessible; falling back would
// silently hand the caller a pointer that the SPI DMA engine cannot use,
// causing silent data corruption. Callers MUST check for NULL.
void *bb_malloc_dma(size_t size);

#ifdef __cplusplus
}
#endif
