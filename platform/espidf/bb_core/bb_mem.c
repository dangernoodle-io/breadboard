// bb_mem — ESP-IDF SPIRAM-preferred allocation helpers.
// Try the SPIRAM/8-bit heap first; fall back to the default (internal) heap so
// boards without PSRAM keep their original behaviour. Frees via heap_caps_free.
//
// Stats instrumentation (BB_MEM_STATS_ENABLE): nine C11 _Atomic counters in
// BSS. Instrumentation compiles out entirely when the feature is disabled.

#include "bb_mem.h"
#include "esp_heap_caps.h"

#if BB_MEM_STATS_ENABLE
#include <stdatomic.h>
#include <stdbool.h>
#include <inttypes.h>

static _Atomic size_t   s_outstanding       = 0;
static _Atomic size_t   s_peak              = 0;
static _Atomic uint32_t s_alloc_count       = 0;
static _Atomic uint32_t s_free_count        = 0;
static _Atomic uint32_t s_alloc_fail        = 0;
static _Atomic uint32_t s_spiram_count      = 0;
static _Atomic size_t   s_spiram_bytes      = 0;
static _Atomic uint32_t s_internal_count    = 0;
static _Atomic size_t   s_internal_bytes    = 0;

static void track_alloc(void *p, bool is_spiram)
{
    atomic_fetch_add_explicit(&s_alloc_count, 1u, memory_order_relaxed);
    if (!p) {
        atomic_fetch_add_explicit(&s_alloc_fail, 1u, memory_order_relaxed);
        return;
    }
    size_t actual = heap_caps_get_allocated_size(p);
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
    if (is_spiram) {
        atomic_fetch_add_explicit(&s_spiram_count, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_spiram_bytes, actual, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&s_internal_count, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_internal_bytes, actual, memory_order_relaxed);
    }
}

static void track_free(void *p)
{
    if (!p) {
        return;
    }
    size_t actual = heap_caps_get_allocated_size(p);
    if (actual == 0) {
        return;
    }
    atomic_fetch_add_explicit(&s_free_count, 1u, memory_order_relaxed);
    atomic_fetch_sub_explicit(&s_outstanding, actual, memory_order_relaxed);
}

// Realloc accounting: adjust outstanding and per-heap-domain subtotals.
//
// old_size and old_is_spiram must be captured BEFORE the realloc call.
// On NULL return (failure) the original block is still live — no outstanding
// change (original tracking stays valid until freed).
//
// When heap_caps_realloc moves the block across heaps (e.g. internal→spiram),
// the old domain's subtotal is decremented by old_size and the new domain's
// subtotal is incremented by new_actual, keeping per-type bytes consistent
// with the post-realloc reality.
static void track_realloc(size_t old_size, bool old_is_spiram, void *new_ptr)
{
    atomic_fetch_add_explicit(&s_alloc_count, 1u, memory_order_relaxed);
    if (!new_ptr) {
        atomic_fetch_add_explicit(&s_alloc_fail, 1u, memory_order_relaxed);
        return;
    }
    size_t new_actual  = heap_caps_get_allocated_size(new_ptr);
    bool   new_spiram  = esp_ptr_external_ram(new_ptr);

    // Subtract old contribution from outstanding and old heap-domain subtotal.
    if (old_size > 0) {
        atomic_fetch_sub_explicit(&s_outstanding, old_size, memory_order_relaxed);
        if (old_is_spiram) {
            atomic_fetch_sub_explicit(&s_spiram_bytes, old_size, memory_order_relaxed);
        } else {
            atomic_fetch_sub_explicit(&s_internal_bytes, old_size, memory_order_relaxed);
        }
    }
    // Add new contribution to outstanding, update peak, and credit new domain.
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
        if (new_spiram) {
            atomic_fetch_add_explicit(&s_spiram_count, 1u, memory_order_relaxed);
            atomic_fetch_add_explicit(&s_spiram_bytes, new_actual, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&s_internal_count, 1u, memory_order_relaxed);
            atomic_fetch_add_explicit(&s_internal_bytes, new_actual, memory_order_relaxed);
        }
    }
}
#endif // BB_MEM_STATS_ENABLE

// ---------------------------------------------------------------------------

void *bb_malloc_prefer_spiram(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if BB_MEM_STATS_ENABLE
    bool spiram_ok = (p != NULL);
#endif
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    }
#if BB_MEM_STATS_ENABLE
    track_alloc(p, spiram_ok);
#endif
    return p;
}

void *bb_calloc_prefer_spiram(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if BB_MEM_STATS_ENABLE
    bool spiram_ok = (p != NULL);
#endif
    if (!p) {
        p = heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT);
    }
#if BB_MEM_STATS_ENABLE
    track_alloc(p, spiram_ok);
#endif
    return p;
}

void *bb_realloc_prefer_spiram(void *ptr, size_t new_size)
{
#if BB_MEM_STATS_ENABLE
    size_t old_size      = ptr ? heap_caps_get_allocated_size(ptr) : 0;
    bool   old_is_spiram = ptr ? esp_ptr_external_ram(ptr) : false;
#endif
    void *p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_DEFAULT);
    }
#if BB_MEM_STATS_ENABLE
    track_realloc(old_size, old_is_spiram, p);
#endif
    return p;
}

void *bb_malloc_internal(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if BB_MEM_STATS_ENABLE
    track_alloc(p, /*is_spiram=*/false);
#endif
    return p;
}

void *bb_calloc_internal(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if BB_MEM_STATS_ENABLE
    track_alloc(p, /*is_spiram=*/false);
#endif
    return p;
}

void *bb_malloc_dma(size_t size)
{
    // Hard fail — no fallback. PSRAM is not DMA-accessible on classic ESP32;
    // falling back to a non-DMA heap would give the caller a pointer that the
    // SPI DMA engine cannot use, causing silent data corruption.
    void *p = heap_caps_malloc(size, MALLOC_CAP_DMA);
#if BB_MEM_STATS_ENABLE
    track_alloc(p, /*is_spiram=*/false);
#endif
    return p;
}

void bb_mem_free(void *p)
{
#if BB_MEM_STATS_ENABLE
    track_free(p);
#endif
    heap_caps_free(p);
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
