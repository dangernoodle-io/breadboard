// bb_meminfo_heap_snap -- the format-agnostic heap-snapshot descriptor SSOT,
// owned by bb_meminfo. See bb_meminfo_heap_snap.h for the snapshot-struct
// contract. Compiles on both host and ESP-IDF; no platform-specific code.
// examples/floor's GET /api/diag/meminfo route is a real consumer of this
// descriptor via bb_data (see floor_app.c) -- the JSON field names/types
// below are now a consumer-visible wire contract; renaming a field is an API
// break, not a free refactor.

#include "bb_meminfo_heap_snap.h"

#include "bb_meminfo.h"

#include <stddef.h>

static const bb_serialize_field_t s_meminfo_region_heap_snap_fields[] = {
    { .key = "free", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_region_t, free) },
    { .key = "min_ever_free", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_region_t, min_ever_free) },
    { .key = "largest_free_block", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_region_t, largest_free_block) },
    { .key = "total", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_region_t, total) },
};
#define BB_MEMINFO_REGION_N_FIELDS \
    (sizeof(s_meminfo_region_heap_snap_fields) / sizeof(s_meminfo_region_heap_snap_fields[0]))

static const bb_serialize_field_t s_meminfo_heap_snap_fields[] = {
    { .key = "default", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_meminfo_heap_snap_t, default_region),
      .children = s_meminfo_region_heap_snap_fields, .n_children = BB_MEMINFO_REGION_N_FIELDS },
    { .key = "internal", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_meminfo_heap_snap_t, internal),
      .children = s_meminfo_region_heap_snap_fields, .n_children = BB_MEMINFO_REGION_N_FIELDS },
    { .key = "dma", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_meminfo_heap_snap_t, dma),
      .children = s_meminfo_region_heap_snap_fields, .n_children = BB_MEMINFO_REGION_N_FIELDS },
    { .key = "spiram", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_meminfo_heap_snap_t, spiram),
      .children = s_meminfo_region_heap_snap_fields, .n_children = BB_MEMINFO_REGION_N_FIELDS },
    { .key = "esp_min_free_heap", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_t, esp_min_free_heap) },
    { .key = "mem_outstanding_bytes", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_t, mem_outstanding_bytes) },
    { .key = "mem_peak_outstanding", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_t, mem_peak_outstanding) },
    { .key = "mem_alloc_fail", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_t, mem_alloc_fail) },
    { .key = "rtc_used", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_t, rtc_used) },
    { .key = "rtc_total", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_t, rtc_total) },
    { .key = "dram_static_bytes", .type = BB_TYPE_U64,
      .offset = offsetof(bb_meminfo_heap_snap_t, dram_static_bytes) },
};

const bb_serialize_desc_t bb_meminfo_heap_snap_desc = {
    .type_name = "meminfo",
    .fields    = s_meminfo_heap_snap_fields,
    .n_fields  = sizeof(s_meminfo_heap_snap_fields) / sizeof(s_meminfo_heap_snap_fields[0]),
    .snap_size = sizeof(bb_meminfo_heap_snap_t),
};

bb_err_t bb_meminfo_heap_snap_fill(bb_meminfo_heap_snap_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;

    bb_meminfo_snapshot_t snap;
    bb_err_t rc = bb_meminfo_get(&snap);
    if (rc != BB_OK) return rc;  // LCOV_EXCL_BR_LINE -- bb_meminfo_get() only fails on a NULL out, never passed here

    struct {
        bb_meminfo_heap_snap_region_t *dst;
        const bb_meminfo_region_t     *src;
    } regions[] = {
        { &out->default_region, &snap.default_region },
        { &out->internal,       &snap.internal },
        { &out->dma,            &snap.dma },
        { &out->spiram,         &snap.spiram },
    };
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        regions[i].dst->free               = (uint64_t)regions[i].src->free;
        regions[i].dst->min_ever_free      = (uint64_t)regions[i].src->min_ever_free;
        regions[i].dst->largest_free_block = (uint64_t)regions[i].src->largest_free_block;
        regions[i].dst->total              = (uint64_t)regions[i].src->total;
    }

    out->esp_min_free_heap = (uint64_t)snap.esp_min_free_heap;

    out->mem_outstanding_bytes = (uint64_t)snap.mem_outstanding_bytes;
    out->mem_peak_outstanding  = (uint64_t)snap.mem_peak_outstanding;
    out->mem_alloc_fail        = (uint64_t)snap.mem_alloc_fail;

    out->rtc_used  = (uint64_t)snap.rtc_used;
    out->rtc_total = (uint64_t)snap.rtc_total;

    out->dram_static_bytes = (uint64_t)snap.dram_static_bytes;

    return BB_OK;
}
