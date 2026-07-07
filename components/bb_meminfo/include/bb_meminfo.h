#pragma once

// bb_meminfo — canonical system-heap reader SSOT (KB #698/#699/#693).
//
// This is the ONE place in breadboard that calls heap_caps_get_free_size /
// heap_caps_get_minimum_free_size / heap_caps_get_largest_free_block /
// heap_caps_get_total_size / esp_get_minimum_free_heap_size. Every other
// consumer (bb_board's accessors, bb_diag, bb_ota_boot, bb_ota_pull,
// bb_sink_http, ...) either delegates to bb_meminfo_get() or is a
// SSOT-consolidation follow-up (see bb_meminfo's README / KB #699).
//
// bb_meminfo is a pure READER — it does not allocate. The allocator facade
// (bb_malloc_prefer_spiram / bb_mem_get_stats / ...) stays in bb_core
// (bb_mem.h); this component only surfaces its stats snapshot alongside the
// raw heap_caps regions. It owns no routes, no bb_pub source, and needs no
// BB_INIT_REGISTER hook — call bb_meminfo_get() on demand.
//
// Platform coverage: ESP-IDF backs this with real heap_caps_* reads; host
// zeros every field (no heap_caps equivalent on host).

#include <stddef.h>
#include <stdint.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// One heap_caps region snapshot (a single MALLOC_CAP_* mask).
typedef struct {
    size_t free;               // heap_caps_get_free_size
    size_t min_ever_free;      // heap_caps_get_minimum_free_size (watermark)
    size_t largest_free_block; // heap_caps_get_largest_free_block
    size_t total;              // heap_caps_get_total_size
} bb_meminfo_region_t;

typedef struct {
    // MALLOC_CAP_DEFAULT — PSRAM-inclusive general-purpose heap. Mirrors
    // bb_board_heap_free_total / heap_minimum_ever / heap_largest_free_block.
    bb_meminfo_region_t default_region;

    // MALLOC_CAP_INTERNAL. largest_free_block uses
    // MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT (value-preserving match of
    // bb_board_heap_internal_largest_free_block's original cap set).
    bb_meminfo_region_t internal;

    // MALLOC_CAP_DMA — DMA-capable heap. Not currently surfaced by any
    // bb_board accessor; new in this pass.
    bb_meminfo_region_t dma;

    // MALLOC_CAP_SPIRAM. All fields 0 on boards with no PSRAM.
    bb_meminfo_region_t spiram;

    // esp_get_minimum_free_heap_size() — IDF's classic overall watermark,
    // distinct from heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    // kept alongside default_region.min_ever_free rather than folded into it.
    size_t esp_min_free_heap;

    // bb_mem facade stats (bb_core, bb_mem.h). See bb_mem.h's STATS COVERAGE
    // CAVEAT: only allocations through bb_mem facade functions are counted.
    size_t   mem_outstanding_bytes;
    size_t   mem_peak_outstanding;
    uint32_t mem_alloc_fail;

    // RTC slow memory — static partition, not a heap. Mirrors
    // bb_board_rtc_used / bb_board_rtc_total.
    size_t rtc_used;
    size_t rtc_total;

    // Internal DRAM static (.data + .bss) bytes at link time. Mirrors
    // bb_board_dram_static_bytes.
    size_t dram_static_bytes;
} bb_meminfo_snapshot_t;

// Populate out with a fresh system-heap snapshot. Returns BB_OK on success,
// BB_ERR_INVALID_ARG if out is NULL. On host, out is zero-filled (BB_OK).
bb_err_t bb_meminfo_get(bb_meminfo_snapshot_t *out);

// Format a compact HEAP-ONLY diagnostic line from snap into buf (snprintf
// semantics — buf is always NUL-terminated when len > 0). Pure formatting,
// no I/O, no allocation; identical on host and ESP-IDF. Fields (in emitted
// order): internal free / min-ever-free / largest-free-block, spiram free,
// dma free, esp_min_free_heap. Deliberately HEAP-ONLY — board/flash/app-size
// fields belong to a different domain (bb_board/build), out of scope here.
//
// Returns the number of bytes that would have been written — matches
// snprintf's buffer/truncation semantics for len>0 (may exceed len on
// truncation); returns 0 (without calling snprintf) when snap or buf is
// NULL or len == 0.
int bb_meminfo_format(const bb_meminfo_snapshot_t *snap, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
