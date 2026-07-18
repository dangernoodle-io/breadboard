#pragma once

// bb_diag_boot_wire — PUBLIC bb_serialize_desc_t (SSOT) for the "diag.boot"
// bb_cache topic (B1-1045 PR-2, cutover composition-root ownership decision
// KB 1454). ADDITIVE-only, INERT: no bb_data_bind() call exists anywhere in
// this PR -- the composition root (PR-4) is the sole owner of wiring this
// descriptor to bb_data.
//
// Mirrors bb_diag_boot_serialize()'s NESTED shape (bb_diag_event_priv.h)
// field-for-field:
//   {"reset_reason","wdt_resets","panic":{"available"[,"boots_since"]},
//    "pending_verify","rolled_back",
//    "reboot_reason":{"source"[,"detail"],"uptime_s","epoch_s"[,"age_s"]},
//    "reboot_history":[{"source","epoch_s","uptime_s"}, ...]}  // newest-first
//
// Every u32 numeric field in bb_diag_boot_snap_t is widened to a fixed
// int64_t here -- bb_serialize_walk()'s BB_TYPE_I64 case always memcpy()s a
// fixed 8 bytes at the descriptor offset (see bb_serialize_walk.c); pointing
// a BB_TYPE_I64 field at a narrower uint32_t would read past it. The
// `source` fields hold the ALREADY-RESOLVED bb_reset_source_str() string
// (bb_reboot_reason.h) rather than the raw uint8_t enum byte, since the
// walker has no notion of an enum-to-string mapping function.
//
// reboot_history is FULLY MATERIALIZED newest-first by the gather (see
// bb_diag_boot_gather() below) into a fixed BB_REBOOT_HISTORY_CAP-element
// array, replicating bb_diag_boot_serialize()'s modular-index walk --
// bb_serialize_walk()'s BB_TYPE_ARR only walks stored (linear) order, it has
// no notion of a ring buffer's head/count.

#include "bb_serialize.h"

#include "bb_core.h"
#include "bb_reboot_reason.h"

#include <stdbool.h>
#include <stdint.h>

// "panic" nested object.
typedef struct {
    bool    available;
    int64_t boots_since;  // meaningful only when available == true
} bb_diag_panic_wire_t;

_Static_assert(sizeof(((bb_diag_panic_wire_t *)0)->boots_since) == 8,
               "bb_diag_panic_wire_t.boots_since must be exactly 8 bytes for BB_TYPE_I64");

// "reboot_reason" nested object.
typedef struct {
    char    source[24];   // bb_reset_source_str() result; longest today ==
                           // "wifi_pending_revert" (20 chars)
    char    detail[49];   // matches bb_reboot_record_t.detail; may be empty
    int64_t uptime_s;
    int64_t epoch_s;
    int64_t age_s;         // meaningful only when age_s_valid == true
    bool    age_s_valid;   // precomputed by the gather -- the walker never
                            // derives values, so the 3-way "both known-good"
                            // condition (bb_diag_boot_serialize()'s guard)
                            // must be resolved before the walk, not during it
} bb_diag_reboot_reason_wire_t;

_Static_assert(sizeof(((bb_diag_reboot_reason_wire_t *)0)->uptime_s) == 8,
               "bb_diag_reboot_reason_wire_t.uptime_s must be exactly 8 bytes for BB_TYPE_I64");
_Static_assert(sizeof(((bb_diag_reboot_reason_wire_t *)0)->epoch_s) == 8,
               "bb_diag_reboot_reason_wire_t.epoch_s must be exactly 8 bytes for BB_TYPE_I64");
_Static_assert(sizeof(((bb_diag_reboot_reason_wire_t *)0)->age_s) == 8,
               "bb_diag_reboot_reason_wire_t.age_s must be exactly 8 bytes for BB_TYPE_I64");

// One "reboot_history" array element.
typedef struct {
    char    source[24];
    int64_t epoch_s;
    int64_t uptime_s;
} bb_diag_reboot_hist_wire_t;

_Static_assert(sizeof(((bb_diag_reboot_hist_wire_t *)0)->epoch_s) == 8,
               "bb_diag_reboot_hist_wire_t.epoch_s must be exactly 8 bytes for BB_TYPE_I64");
_Static_assert(sizeof(((bb_diag_reboot_hist_wire_t *)0)->uptime_s) == 8,
               "bb_diag_reboot_hist_wire_t.uptime_s must be exactly 8 bytes for BB_TYPE_I64");

// Root.
typedef struct {
    char                          reset_reason[16];
    int64_t                       wdt_resets;
    bb_diag_panic_wire_t          panic;
    bool                          pending_verify;
    bool                          rolled_back;
    bb_diag_reboot_reason_wire_t  reboot_reason;

    // Materialized newest-first by the gather (see bb_diag_boot_gather());
    // `reboot_history` is the BB_TYPE_ARR carrier pointing at
    // `reboot_history_items`, .count <= BB_REBOOT_HISTORY_CAP.
    bb_diag_reboot_hist_wire_t    reboot_history_items[BB_REBOOT_HISTORY_CAP];
    bb_serialize_arr_t            reboot_history;
} bb_diag_boot_wire_t;

_Static_assert(sizeof(((bb_diag_boot_wire_t *)0)->wdt_resets) == 8,
               "bb_diag_boot_wire_t.wdt_resets must be exactly 8 bytes for BB_TYPE_I64");

extern const bb_serialize_desc_t bb_diag_boot_wire_desc;

// Portable (no ESP-IDF dep): reads the diag.boot bb_cache entry via
// bb_cache_get_raw() and widens/materializes it into `dst` -- see the
// per-field widening + reboot_history materialization notes above. Returns
// BB_ERR_INVALID_ARG if dst is NULL; otherwise propagates bb_cache_get_raw()'s
// own return (BB_ERR_NOT_FOUND if the diag.boot key isn't registered yet).
bb_err_t bb_diag_boot_gather(bb_diag_boot_wire_t *dst);
