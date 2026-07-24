#pragma once

// bb_diag_storage_partitions -- the "storage/partitions" bb_diag section
// (GET /api/diag/storage/partitions), part of the storage-inventory
// cluster (B1-767 PR9). Wraps bb_partition_list() (already portable) +
// bb_partition_row_wire_from_info() (PR8's U64-widening helper) behind the
// bb_diag section registry -- no new partition-reading logic, just a
// bb_serialize_desc_t over PR8's bb_partition_row_fields.
//
// bb_diag_storage_partitions_fill() is pure/portable -- directly
// host-callable against the host bb_partition mock, so host tests exercise
// the exact production code path. bb_diag_storage_partitions_register() is
// the only ESP-IDF-gated symbol here (registration is a composition-root
// concern deferred to the floor/HW-validation PRs per this PR's scope).

#include "bb_diag_section.h"
#include "bb_partition_serialize.h"
#include "bb_serialize.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capacity (Kconfig bridge -- pattern from bb_storage.h/bb_diag_section_priv.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_DIAG_STORAGE_PARTITIONS_ROW_CAP
#define BB_DIAG_STORAGE_PARTITIONS_ROW_CAP CONFIG_BB_DIAG_STORAGE_PARTITIONS_ROW_CAP
#endif
#ifndef BB_DIAG_STORAGE_PARTITIONS_ROW_CAP
#define BB_DIAG_STORAGE_PARTITIONS_ROW_CAP 16
#endif

// Section snapshot. `rows`/`rows_items` follow the same storage/carrier
// split as bb_diag_storage_nvs_snap_t's entries (see that header) --
// `rows_items` is the backing bb_partition_row_wire_t storage, `rows` is
// the bb_serialize_arr_t the descriptor's BB_TYPE_ARR field points at.
typedef struct {
    bb_partition_row_wire_t rows_items[BB_DIAG_STORAGE_PARTITIONS_ROW_CAP];
    bb_serialize_arr_t      rows;
    uint64_t                row_count;  // TRUE total (may exceed the cap)
} bb_diag_storage_partitions_snap_t;

extern const bb_serialize_desc_t bb_diag_storage_partitions_desc;

// Hand-authored JSON Schema for the section's GET response (B1-1180 PR-1)
// -- makes "storage/partitions" VISIBLE to bb_openapi_emit() via
// bb_diag_section_t.describe_route (wired in
// components/bb_diag/bb_diag_storage_partitions.c's
// bb_diag_storage_partitions_register()). On-device (NOT host-gated). See
// test/test_host/test_bb_diag_storage_partitions_meta_golden.c for the
// byte-fidelity proof against bb_diag_storage_partitions_meta.
extern const char *const bb_diag_storage_partitions_schema;

// bb_serialize_desc_meta_t companion (B1-1180 PR-1) -- co-located JSON
// Schema docs/validation table for bb_diag_storage_partitions_desc above,
// proving bb_diag_storage_partitions_schema's byte-fidelity. Host-only (see
// components/bb_ws_server/include/bb_ws_server_diag.h's doc for the
// BB_SERIALIZE_META_HOST mechanism).
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_diag_storage_partitions_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// Fill hook (bb_diag_fill_fn signature) -- pure/portable, `args->query` is
// always NULL for this section (no query_keys declared). Calls
// bb_partition_list() then widens each row via
// bb_partition_row_wire_from_info(). Returns BB_ERR_INVALID_ARG if dst is
// NULL; otherwise propagates bb_partition_list()'s own return.
bb_err_t bb_diag_storage_partitions_fill(void *dst, const bb_diag_fill_args_t *args);

#ifdef ESP_PLATFORM
// Registers this section as "storage/partitions" (GET
// /api/diag/storage/partitions) via bb_diag_register_section(). Composition-
// time-only, once. Previously floor-handwire-only; now also codegen-visible
// (B1-1077 PR-3a) so smoke keeps partition-table reachability after the
// legacy /api/diag/partitions exact route (bb_partition_routes.c) is
// deleted -- see bb_partition.h's doc comment.
// bbtool:init tier=regular fn=bb_diag_storage_partitions_register
bb_err_t bb_diag_storage_partitions_register(void);
#endif

#ifdef __cplusplus
}
#endif
