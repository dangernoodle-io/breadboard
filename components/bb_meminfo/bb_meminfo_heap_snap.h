#pragma once

// Private: format-agnostic heap-snapshot descriptor, owned by bb_meminfo
// (B1-767-family, additive-only -- not wired into any consumer yet). This is
// the ONE heap snapshot source: any future serializer/emitter (JSON HTTP
// route, console line, etc.) walks this same descriptor rather than
// hand-rolling its own subset.
//
// bb_meminfo_snapshot_t (bb_meminfo.h) is NOT reused directly as the
// snapshot struct: its bb_meminfo_region_t fields are `size_t`, whose width
// varies by platform (4 bytes on esp32, 8 on a 64-bit host build), but
// bb_serialize_walk()'s BB_TYPE_U64 case always memcpy()s a fixed 8 bytes at
// the descriptor offset (see bb_serialize_walk.c) -- pointing a BB_TYPE_U64
// field at a 4-byte size_t would read 4 bytes past it. bb_meminfo_heap_snap_t
// instead widens every numeric field to a fixed uint64_t, keeping the
// descriptor portable regardless of the platform's size_t width.

#include "bb_serialize.h"

#include "bb_core.h"

#include <stdint.h>

// One heap_caps region, widened to uint64_t -- mirrors bb_meminfo_region_t
// field-for-field (bb_meminfo.h).
typedef struct {
    uint64_t free;
    uint64_t min_ever_free;
    uint64_t largest_free_block;
    uint64_t total;
} bb_meminfo_heap_snap_region_t;

// Root -- mirrors bb_meminfo_snapshot_t field-for-field (bb_meminfo.h),
// widened to fixed uint64_t.
typedef struct {
    bb_meminfo_heap_snap_region_t default_region;
    bb_meminfo_heap_snap_region_t internal;
    bb_meminfo_heap_snap_region_t dma;
    bb_meminfo_heap_snap_region_t spiram;

    uint64_t esp_min_free_heap;

    uint64_t mem_outstanding_bytes;
    uint64_t mem_peak_outstanding;
    uint64_t mem_alloc_fail;

    uint64_t rtc_used;
    uint64_t rtc_total;

    uint64_t dram_static_bytes;
} bb_meminfo_heap_snap_t;

extern const bb_serialize_desc_t bb_meminfo_heap_snap_desc;

// Populates `out` from a fresh bb_meminfo_get() snapshot, widening every
// field to bb_meminfo_heap_snap_t's fixed uint64_t layout. Returns
// BB_ERR_INVALID_ARG if `out` is NULL; otherwise propagates bb_meminfo_get()'s
// own return (BB_OK on host -- bb_meminfo_get() zero-fills there). Portable:
// no ESP_PLATFORM gate needed, bb_meminfo_get() is already host-safe.
bb_err_t bb_meminfo_heap_snap_fill(bb_meminfo_heap_snap_t *out);
