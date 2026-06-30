// bb_mem — host backend. No SPIRAM on host; plain malloc/calloc/free.
//
// Stats instrumentation (BB_MEM_STATS_ENABLE): nine C11 _Atomic counters in
// BSS, using malloc_size (macOS) / malloc_usable_size (Linux) for size-on-free
// without a side table.  Instrumentation compiles out entirely when disabled.
//
// Test hook (BB_MEM_TESTING): replaces the underlying allocator for
// bb_malloc/bb_calloc/bb_realloc variants.  See bb_mem_test.h.

#include "bb_mem.h"
#include <stdlib.h>
#include <string.h>

#if BB_MEM_STATS_ENABLE
#include <stdatomic.h>
#include <stdbool.h>
#include <inttypes.h>
#if defined(__APPLE__)
#  include <malloc/malloc.h>
#  define bb_mem_block_size(p) malloc_size(p)
#else
#  include <malloc.h>
#  define bb_mem_block_size(p) malloc_usable_size(p)
#endif

static _Atomic size_t   s_outstanding    = 0;
static _Atomic size_t   s_peak           = 0;
static _Atomic uint32_t s_alloc_count    = 0;
static _Atomic uint32_t s_free_count     = 0;
static _Atomic uint32_t s_alloc_fail     = 0;
static _Atomic uint32_t s_spiram_count   = 0;
static _Atomic size_t   s_spiram_bytes   = 0;
static _Atomic uint32_t s_internal_count = 0;
static _Atomic size_t   s_internal_bytes = 0;

static void track_alloc(void *p)
{
    atomic_fetch_add_explicit(&s_alloc_count, 1u, memory_order_relaxed);
    if (!p) {
        atomic_fetch_add_explicit(&s_alloc_fail, 1u, memory_order_relaxed);
        return;
    }
    size_t actual = bb_mem_block_size(p);
    if (actual == 0) {
        return;
    }
    size_t prev = atomic_fetch_add_explicit(&s_outstanding, actual, memory_order_relaxed);
    size_t cur  = prev + actual;
    size_t peak = atomic_load_explicit(&s_peak, memory_order_relaxed);
    while (cur > peak) {
        if (atomic_compare_exchange_weak_explicit(&s_peak, &peak, cur,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            break;
        }
    }
    // Host: always internal (no SPIRAM)
    atomic_fetch_add_explicit(&s_internal_count, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_internal_bytes, actual, memory_order_relaxed);
    (void)s_spiram_count;
    (void)s_spiram_bytes;
}

static void track_free(void *p)
{
    if (!p) {
        return;
    }
    size_t actual = bb_mem_block_size(p);
    if (actual == 0) {
        return;
    }
    atomic_fetch_add_explicit(&s_free_count, 1u, memory_order_relaxed);
    atomic_fetch_sub_explicit(&s_outstanding, actual, memory_order_relaxed);
}

// Realloc accounting: adjust outstanding by (new - old) and track new block.
// old_size must be captured via bb_mem_block_size(ptr) BEFORE the realloc call.
// On NULL return (failure) the original block is still live — caller must not
// change outstanding (original tracking stays valid until freed).
static void track_realloc(size_t old_size, void *new_ptr)
{
    atomic_fetch_add_explicit(&s_alloc_count, 1u, memory_order_relaxed);
    if (!new_ptr) {
        atomic_fetch_add_explicit(&s_alloc_fail, 1u, memory_order_relaxed);
        return;
    }
    size_t new_actual = bb_mem_block_size(new_ptr);
    // Subtract old contribution from outstanding.
    if (old_size > 0) {
        atomic_fetch_sub_explicit(&s_outstanding, old_size, memory_order_relaxed);
    }
    // Add new contribution to outstanding and update peak.
    if (new_actual > 0) {
        size_t prev = atomic_fetch_add_explicit(&s_outstanding, new_actual, memory_order_relaxed);
        size_t cur  = prev + new_actual;
        size_t peak = atomic_load_explicit(&s_peak, memory_order_relaxed);
        while (cur > peak) {
            if (atomic_compare_exchange_weak_explicit(&s_peak, &peak, cur,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                break;
            }
        }
        // Host: always internal (no SPIRAM)
        atomic_fetch_add_explicit(&s_internal_count, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_internal_bytes, new_actual, memory_order_relaxed);
    }
}
#endif // BB_MEM_STATS_ENABLE

// ---------------------------------------------------------------------------
// Testing hook
// ---------------------------------------------------------------------------

#ifdef BB_MEM_TESTING
static void *(*s_alloc_hook)(size_t) = NULL;

void bb_mem_set_alloc_hook(void *(*fn)(size_t))
{
    s_alloc_hook = fn;
}
#endif

// ---------------------------------------------------------------------------

void *bb_malloc_prefer_spiram(size_t size)
{
#ifdef BB_MEM_TESTING
    void *p = s_alloc_hook ? s_alloc_hook(size) : malloc(size);
#else
    void *p = malloc(size);
#endif
#if BB_MEM_STATS_ENABLE
    track_alloc(p);
#endif
    return p;
}

void *bb_calloc_prefer_spiram(size_t n, size_t size)
{
#ifdef BB_MEM_TESTING
    void *p;
    if (s_alloc_hook) {
        p = s_alloc_hook(n * size);
    } else {
        p = calloc(n, size);
    }
#else
    void *p = calloc(n, size);
#endif
#if BB_MEM_STATS_ENABLE
    track_alloc(p);
#endif
    return p;
}

void *bb_realloc_prefer_spiram(void *ptr, size_t new_size)
{
#if BB_MEM_STATS_ENABLE
    size_t old_size = ptr ? bb_mem_block_size(ptr) : 0;
#endif
#ifdef BB_MEM_TESTING
    // When the hook is set it stands in for the entire realloc: a NULL return
    // simulates failure (original ptr stays valid and tracked).
    void *p = s_alloc_hook ? s_alloc_hook(new_size) : realloc(ptr, new_size);
#else
    void *p = realloc(ptr, new_size);
#endif
#if BB_MEM_STATS_ENABLE
    track_realloc(old_size, p);
#endif
    return p;
}

void *bb_malloc_internal(size_t size)
{
#ifdef BB_MEM_TESTING
    void *p = s_alloc_hook ? s_alloc_hook(size) : malloc(size);
#else
    void *p = malloc(size);
#endif
#if BB_MEM_STATS_ENABLE
    track_alloc(p);
#endif
    return p;
}

void *bb_calloc_internal(size_t n, size_t size)
{
#ifdef BB_MEM_TESTING
    void *p;
    if (s_alloc_hook) {
        p = s_alloc_hook(n * size);
        // The hook returns malloc-semantics (uninitialised) memory; enforce the
        // calloc zero-init contract so callers observe zeroed bytes even under injection.
        if (p) { memset(p, 0, n * size); }
    } else {
        p = calloc(n, size);
    }
#else
    void *p = calloc(n, size);
#endif
#if BB_MEM_STATS_ENABLE
    track_alloc(p);
#endif
    return p;
}

void *bb_malloc_dma(size_t size)
{
    // Host: no DMA capability concept — plain malloc suffices for host tests.
    // The no-fallback contract is enforced only on ESP-IDF (see espidf backend).
#ifdef BB_MEM_TESTING
    void *p = s_alloc_hook ? s_alloc_hook(size) : malloc(size);
#else
    void *p = malloc(size);
#endif
#if BB_MEM_STATS_ENABLE
    track_alloc(p);
#endif
    return p;
}

void bb_mem_free(void *p)
{
#if BB_MEM_STATS_ENABLE
    track_free(p);
#endif
    free(p);
}

void bb_mem_get_stats(bb_mem_stats_t *out)
{
#if BB_MEM_STATS_ENABLE
    out->outstanding_bytes    = atomic_load_explicit(&s_outstanding,    memory_order_relaxed);
    out->peak_outstanding     = atomic_load_explicit(&s_peak,           memory_order_relaxed);
    out->alloc_count          = atomic_load_explicit(&s_alloc_count,    memory_order_relaxed);
    out->free_count           = atomic_load_explicit(&s_free_count,     memory_order_relaxed);
    out->alloc_fail           = atomic_load_explicit(&s_alloc_fail,     memory_order_relaxed);
    out->spiram_alloc_count   = atomic_load_explicit(&s_spiram_count,   memory_order_relaxed);
    out->spiram_alloc_bytes   = atomic_load_explicit(&s_spiram_bytes,   memory_order_relaxed);
    out->internal_alloc_count = atomic_load_explicit(&s_internal_count, memory_order_relaxed);
    out->internal_alloc_bytes = atomic_load_explicit(&s_internal_bytes, memory_order_relaxed);
#else
    *out = (bb_mem_stats_t){0};
#endif
}

void bb_mem_reset_stats(void)
{
#if BB_MEM_STATS_ENABLE
    atomic_store_explicit(&s_outstanding,    0,  memory_order_relaxed);
    atomic_store_explicit(&s_peak,           0,  memory_order_relaxed);
    atomic_store_explicit(&s_alloc_count,    0u, memory_order_relaxed);
    atomic_store_explicit(&s_free_count,     0u, memory_order_relaxed);
    atomic_store_explicit(&s_alloc_fail,     0u, memory_order_relaxed);
    atomic_store_explicit(&s_spiram_count,   0u, memory_order_relaxed);
    atomic_store_explicit(&s_spiram_bytes,   0,  memory_order_relaxed);
    atomic_store_explicit(&s_internal_count, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_internal_bytes, 0,  memory_order_relaxed);
#endif
}
