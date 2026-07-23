#pragma once

// bb_diag_storage_nvs -- the "storage/nvs" bb_diag section (GET
// /api/diag/storage/nvs), part of the storage-inventory cluster
// (B1-767 PR9). Merges LIVE NVS inventory (bb_storage_list_entries()/
// bb_storage_get_stats(), PR5) with the bb_settings SCHEMA overlay (PR7,
// bb_settings_nv_overlay_entries()) by (ns_or_dir, key) string-equality:
// a live entry whose (ns,key) matches a schema row gets has_schema=true
// plus that row's type_str/label/secret/provisioning_only/reboot_required;
// an unmatched entry gets has_schema=false, a static enc-name string for
// type_str, and an absent (NULL/0) label.
//
// Every entry is INCLUDED (not filtered) -- ESP-IDF-system namespaces
// (WiFi's "nvs.net80211", PHY calibration's "phy") are flagged via the
// `system` bool rather than hidden, so the section reports TRUE total NVS
// usage while still letting a UI distinguish app-owned from IDF-owned rows.
//
// `entries` is a BB_ARR_STREAM field (B1-1077 PR-2) -- no fixed row-count
// cap. bb_diag_storage_nvs_iter() is pure/portable (no ESP-IDF type in the
// loop) -- directly host-callable, so host tests exercise the exact
// production code path. bb_diag_storage_nvs_register() is the only
// ESP-IDF-gated symbol here (it calls bb_diag_register_section(), which is
// itself portable, but registration is a composition-root concern deferred
// to the floor/HW-validation PRs per this PR's scope).

#include "bb_diag_section.h"
#include "bb_serialize.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One NVS entry row -- LIVE (ns_or_dir/key/len/system) merged with SCHEMA
// (type_str/label/secret/provisioning_only/reboot_required, only present
// when has_schema).
typedef struct {
    char                  ns_or_dir[16];
    char                  key[16];
    bb_serialize_str_n_t  type_str;  // schema type_str when has_schema, else a
                                      // static enc-name literal ("blob"/"str"/
                                      // "u8"/"u16"/"u32"/"i32")
    bb_serialize_str_n_t  label;     // {NULL,0} when !has_schema
    uint64_t              len;       // widened from bb_storage_entry_t.len (size_t)
    bool                  system;    // true for ESP-IDF-owned namespaces
    bool                  has_schema;
    bool                  secret;
    bool                  provisioning_only;
    bool                  reboot_required;
} bb_diag_storage_nvs_row_t;

// Section snapshot. `entries` is the descriptor's BB_TYPE_ARR
// (cardinality == BB_ARR_STREAM) field's runtime carrier, wired by
// bb_diag_storage_nvs_iter()'s phase 2 to the dispatcher's row arena via
// bb_serialize_arr_stream_from_buf() -- `entries_iter_state` is that call's
// CALLER-OWNED iterator state, kept here (rather than a local in the iter
// fn) because `dst` (this struct) must outlive the walk/render call that
// follows iter's phase-2 return. `entry_count` is the TRUE total row count
// (phase 1's report; unbounded, never capped).
typedef struct {
    bb_serialize_arr_stream_t   entries;
    bb_serialize_arr_buf_iter_t entries_iter_state;
    uint64_t                    entry_count;
    uint64_t                    used_bytes;
    uint64_t                    free_bytes;
    uint64_t                    total_bytes;
    uint64_t                    namespace_count;
} bb_diag_storage_nvs_snap_t;

extern const bb_serialize_desc_t bb_diag_storage_nvs_desc;

// Hand-authored JSON Schema for the section's GET response (B1-1180 PR-1) --
// makes "storage/nvs" VISIBLE to bb_openapi_emit() via
// bb_diag_section_t.describe_route (wired in this file's own
// bb_diag_storage_nvs_register()). On-device (NOT host-gated). See
// test/test_host/test_bb_diag_storage_nvs_meta_golden.c for the
// byte-fidelity proof against bb_diag_storage_nvs_meta.
extern const char *const bb_diag_storage_nvs_schema;

// bb_serialize_desc_meta_t companion (B1-1180 PR-1) -- co-located JSON
// Schema docs/validation table for bb_diag_storage_nvs_desc above, proving
// bb_diag_storage_nvs_schema's byte-fidelity. Host-only (see
// components/bb_ws_server/include/bb_ws_server_diag.h's doc for the
// BB_SERIALIZE_META_HOST mechanism).
#if defined(BB_SERIALIZE_META_HOST)
#include "bb_serialize_meta.h"

extern const bb_serialize_desc_meta_t bb_diag_storage_nvs_meta;
#endif /* BB_SERIALIZE_META_HOST */

// Two-phase iter hook (bb_diag_iter_fn signature) -- pure/portable,
// `args->query` is always NULL for this section (no query_keys declared).
//
// Phase 1 (row_arena == NULL, row_cap == 0): populates `dst`'s scalar
// fields (used_bytes/free_bytes/total_bytes/namespace_count/entry_count)
// from bb_storage's "nvs" backend and reports the TRUE live entry count via
// `*row_count`; wires `dst->entries` to an empty stream (defensive -- the
// dispatcher never renders a phase-1-only `dst`).
//
// Phase 2 (row_arena != NULL, row_cap = phase 1's reported count):
// re-enumerates live entries (bounded by `row_cap`) into a section-owned
// temporary staging buffer, merges each against the bb_settings schema
// overlay (byte-identical merge logic to the prior fixed-cap fill()), and
// composes the resulting bb_diag_storage_nvs_row_t rows into `row_arena`.
// Wires `dst->entries` to the arena via bb_serialize_arr_stream_from_buf()
// and reports the actual row count via `*row_count` (<= row_cap).
//
// Returns whatever bb_storage_list_entries()/bb_storage_get_stats() return
// (e.g. BB_ERR_UNSUPPORTED if no "nvs" backend with list_entries/get_stats
// is registered); BB_ERR_NO_MEM if phase 2's internal staging allocation
// fails; BB_ERR_INVALID_ARG if `dst` or `row_count` is NULL.
bb_err_t bb_diag_storage_nvs_iter(void *dst, void *row_arena, size_t row_cap,
                                   size_t *row_count, const bb_diag_fill_args_t *args);

#ifdef ESP_PLATFORM
// Registers this section as "storage/nvs" (GET /api/diag/storage/nvs) via
// bb_diag_register_section(). NOT called anywhere in this PR -- floor
// wiring + on-device stack-headroom validation are deferred to a later PR.
bb_err_t bb_diag_storage_nvs_register(void);
#endif

#ifdef __cplusplus
}
#endif
