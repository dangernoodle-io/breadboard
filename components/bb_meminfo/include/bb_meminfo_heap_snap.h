#pragma once

// Format-agnostic heap-snapshot descriptor, owned by bb_meminfo
// (B1-767-family, additive-only). Public since B1-1025 (floor's
// GET /api/diag/meminfo route is its first real consumer via bb_data). This
// is the ONE heap snapshot source: any serializer/emitter (JSON HTTP route,
// console line, etc.) walks this same descriptor rather than hand-rolling
// its own subset.
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
    uint64_t allocated;
} bb_meminfo_heap_snap_region_t;

// Root -- mirrors bb_meminfo_snapshot_t field-for-field (bb_meminfo.h),
// widened to fixed uint64_t.
typedef struct {
    bb_meminfo_heap_snap_region_t default_region;
    bb_meminfo_heap_snap_region_t internal;
    bb_meminfo_heap_snap_region_t dma;
    bb_meminfo_heap_snap_region_t spiram;
    bb_meminfo_heap_snap_region_t exec;

    uint64_t esp_min_free_heap;

    uint64_t mem_outstanding_bytes;
    uint64_t mem_peak_outstanding;
    uint64_t mem_alloc_fail;

    uint64_t rtc_used;
    uint64_t rtc_total;

    uint64_t dram_static_bytes;
} bb_meminfo_heap_snap_t;

extern const bb_serialize_desc_t bb_meminfo_heap_snap_desc;

// Hand-authored JSON Schema for the "meminfo" bb_diag section's GET response
// (B1-1180 PR-1) -- makes it VISIBLE to bb_openapi_emit() via the section's
// describe-only route (wired in components/bb_diag/bb_diag_meminfo.c's
// bb_diag_meminfo_register(), see bb_diag_section_t.describe_route's doc
// comment). On-device (NOT host-gated). See
// test/test_host/test_bb_meminfo_heap_snap_meta_golden.c for the
// byte-fidelity proof against bb_meminfo_heap_snap_meta.
extern const char *const bb_meminfo_heap_snap_schema;

// The literal text behind bb_meminfo_heap_snap_schema, as a #define rather
// than only the extern variable above (B1-1180 PR-1 review fix) -- this
// component's "meminfo" section is registered from a DIFFERENT TU
// (components/bb_diag/bb_diag_meminfo.c), whose own PRODUCER-OWNED
// `static const` describe route needs the SAME literal text as a genuine
// compile-time constant expression: `.schema = bb_meminfo_heap_snap_schema`
// (the variable's runtime value) is NOT a valid static/file-scope
// initializer in C ("initializer element is not constant"); the
// macro-expanded string literal is. A plain preprocessor string constant --
// no platform/foreign type, so this doesn't violate the public-header
// portability rule.
#define BB_MEMINFO_HEAP_SNAP_REGION_SCHEMA \
    "{\"type\":\"object\",\"properties\":{" \
    "\"free\":{\"type\":\"integer\"}," \
    "\"min_ever_free\":{\"type\":\"integer\"}," \
    "\"largest_free_block\":{\"type\":\"integer\"}," \
    "\"total\":{\"type\":\"integer\"}," \
    "\"allocated\":{\"type\":\"integer\"}}," \
    "\"required\":[\"free\",\"min_ever_free\",\"largest_free_block\",\"total\",\"allocated\"]," \
    "\"additionalProperties\":false}"

#define BB_MEMINFO_HEAP_SNAP_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"default\":" BB_MEMINFO_HEAP_SNAP_REGION_SCHEMA "," \
    "\"internal\":" BB_MEMINFO_HEAP_SNAP_REGION_SCHEMA "," \
    "\"dma\":" BB_MEMINFO_HEAP_SNAP_REGION_SCHEMA "," \
    "\"spiram\":" BB_MEMINFO_HEAP_SNAP_REGION_SCHEMA "," \
    "\"exec\":" BB_MEMINFO_HEAP_SNAP_REGION_SCHEMA "," \
    "\"esp_min_free_heap\":{\"type\":\"integer\"}," \
    "\"mem_outstanding_bytes\":{\"type\":\"integer\"}," \
    "\"mem_peak_outstanding\":{\"type\":\"integer\"}," \
    "\"mem_alloc_fail\":{\"type\":\"integer\"}," \
    "\"rtc_used\":{\"type\":\"integer\"}," \
    "\"rtc_total\":{\"type\":\"integer\"}," \
    "\"dram_static_bytes\":{\"type\":\"integer\"}}," \
    "\"required\":[\"default\",\"internal\",\"dma\",\"spiram\",\"exec\"," \
    "\"esp_min_free_heap\",\"mem_outstanding_bytes\",\"mem_peak_outstanding\"," \
    "\"mem_alloc_fail\",\"rtc_used\",\"rtc_total\",\"dram_static_bytes\"]," \
    "\"additionalProperties\":false}"

// bb_serialize_desc_meta_t companion (B1-1180 PR-1) -- co-located JSON
// Schema docs/validation table for bb_meminfo_heap_snap_desc above, proving
// bb_meminfo_heap_snap_schema's byte-fidelity. Host-only (see
// components/bb_ws_server/include/bb_ws_server_diag.h's doc for the
// BB_SERIALIZE_META_HOST mechanism).
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_meminfo_heap_snap_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// Populates `out` from a fresh bb_meminfo_get() snapshot, widening every
// field to bb_meminfo_heap_snap_t's fixed uint64_t layout. Returns
// BB_ERR_INVALID_ARG if `out` is NULL; otherwise propagates bb_meminfo_get()'s
// own return (BB_OK on host -- bb_meminfo_get() zero-fills there). Portable:
// no ESP_PLATFORM gate needed, bb_meminfo_get() is already host-safe.
bb_err_t bb_meminfo_heap_snap_fill(bb_meminfo_heap_snap_t *out);
