#pragma once

// bb_ring_diag — the "rings" bb_diag section (GET /api/diag/rings),
// B1-1077 PR-3a. Replaces the prior hand-rolled exact route
// (platform/espidf/bb_ring_diag/bb_ring_diag.c, deleted this PR) with a
// bb_diag_register_section() fill adapter. FIXED-cap array (BB_ARR_FIXED,
// not BB_ARR_STREAM) -- the registry itself is already Kconfig-bounded
// (BB_QUEUE_REGISTRY_MAX, <= 32), so a two-phase iter buys no extra
// unbounded-ness here (unlike storage/nvs's genuinely-unbounded live NVS
// inventory); a fixed embedded array, same shape as
// bb_diag_storage_partitions, is the simpler defensible choice for a small,
// already-capped source.

#include "bb_diag_section.h"
#include "bb_queue.h"
#include "bb_queue_registry.h"
#include "bb_serialize.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capacity: BB_QUEUE_REGISTRY_MAX (Kconfig bridge) is defined once in
// bb_queue_registry.h -- included above -- and reused here to size the fill
// snapshot. Do not re-derive the bridge in this file.
// ---------------------------------------------------------------------------

// One "rings" array row.
typedef struct {
    char    name[BB_QUEUE_NAME_MAX];
    int64_t count;
    int64_t capacity;
    int64_t dropped;
    int64_t truncated;
    int64_t bytes_used;
} bb_ring_diag_row_t;

// Section snapshot.
typedef struct {
    int64_t             count;              // number of rings actually reported
    int64_t             registry_capacity;  // BB_QUEUE_REGISTRY_MAX
    bb_ring_diag_row_t  rings_items[BB_QUEUE_REGISTRY_MAX];
    bb_serialize_arr_t  rings;
} bb_ring_diag_snap_t;

extern const bb_serialize_desc_t bb_ring_diag_desc;

// Hand-authored JSON Schema for the section's GET response (B1-1180 PR-1) --
// makes the "rings" section VISIBLE to bb_openapi_emit() via
// bb_diag_section_t.describe_route (wired in this file's own
// bb_ring_diag_register()). On-device (NOT host-gated). See
// test/test_host/test_bb_ring_diag_meta_golden.c for the byte-fidelity proof
// against bb_ring_diag_meta.
extern const char *const bb_ring_diag_schema;

// Fill hook (bb_diag_fill_fn signature) -- pure/portable, drives
// bb_queue_registry_foreach() under its own lock, copy-out only (see
// bb_queue_registry.h's foreach contract) -- no I/O while the registry lock
// is held. `args` is unused (this section declares no query_keys). Returns
// BB_ERR_INVALID_ARG if dst is NULL.
bb_err_t bb_ring_diag_fill(void *dst, const bb_diag_fill_args_t *args);

// bb_serialize_desc_meta_t companion (B1-1180 PR-1) -- co-located JSON
// Schema docs/validation table for bb_ring_diag_desc above, proving
// bb_ring_diag_schema's byte-fidelity. Host-only (see bb_ws_server_diag.h's
// doc for the BB_SERIALIZE_META_HOST mechanism).
#if defined(BB_SERIALIZE_META_HOST)
#include "bb_serialize_meta.h"

extern const bb_serialize_desc_meta_t bb_ring_diag_meta;
#endif /* BB_SERIALIZE_META_HOST */

#ifdef ESP_PLATFORM
// Registers this section as "rings" (GET /api/diag/rings) via
// bb_diag_register_section(). Composition-time-only, once.
// bbtool:init tier=regular fn=bb_ring_diag_register
bb_err_t bb_ring_diag_register(void);
#endif

#ifdef __cplusplus
}
#endif
