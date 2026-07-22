#pragma once

// bb_diag_boot_wire — PUBLIC bb_serialize_desc_t (SSOT) for the "diag.boot"
// bb_cache topic (B1-1045 PR-2, cutover composition-root ownership decision
// KB 1454), plus the REST GET render seam cut over onto it (B1-1053 PR1).
//
// bb_diag_boot_bind()/bb_diag_boot_render_envelope() below are this PR's
// addition: contrary to this PR's brief (which assumed a production
// bb_data_bind("diag.boot", ...) call already existed from B1-1045), NO
// composition root binds "diag.boot" to bb_data anywhere in this tree --
// only examples/floor's "log" key does (B1-1045 PR-4's own proof-of-concept
// scope; the other 5 dissolved-bb_event producers, including diag.boot, were
// explicitly deferred, see floor_app.c's file header). Without a bind call,
// bb_data_render(key="diag.boot") would return BB_ERR_NOT_FOUND on every
// GET, so this file now self-binds (mirrors bb_ota_check_config_bind()'s
// portable self-bind pattern, components/bb_ota_check/src/bb_ota_check_common.c)
// -- bb_diag_routes_init() (ESP-IDF composition surface) calls
// bb_diag_boot_bind() once, at the same point it registers the bb_cache
// entry.
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

// Binds the "diag.boot" bb_data key against bb_diag_boot_gather() above (see
// the file-header note for why this call exists in this PR). Portable (no
// bb_http_handle_t/ESP-IDF dependency) -- called by bb_diag_routes_init()
// and directly by host tests after bb_data_test_reset(). Idempotent:
// bb_data_bind() re-binding an already-bound key overrides it in place.
bb_err_t bb_diag_boot_bind(void);

// Renders the "diag.boot" bb_data binding onto `req` as a {"ts_ms":N,
// "data":{...}} envelope: "data" is bb_diag_boot_wire_desc's JSON rendering
// (byte-identical to bb_data_render()'s own output), "ts_ms" is
// bb_clock_now_ms64() read at render time -- this key's gather
// (bb_diag_boot_gather()) has no notion of a wire-carried sample time (it
// widens whatever bb_cache currently holds; bb_data_render() itself never
// surfaces a timestamp), so "ts_ms" here means "when this response was
// generated", not "when the underlying value was sampled". Portable
// (compiles host + ESP-IDF; only bb_diag_boot_bind() needs to have already
// run) -- host-testable directly via bb_http_host_capture_begin/end
// (mirrors test_bb_http_json_obj_stream.c).
//
// Returns BB_ERR_INVALID_ARG if `req` is NULL. Otherwise propagates
// bb_data_render()'s own error (e.g. BB_ERR_NOT_FOUND if "diag.boot" isn't
// bound) or any bb_http_resp_json_obj_* stream error.
bb_err_t bb_diag_boot_render_envelope(bb_http_request_t *req);
