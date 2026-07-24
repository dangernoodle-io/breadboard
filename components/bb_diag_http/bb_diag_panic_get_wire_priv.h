#pragma once

// bb_diag_panic_get_wire — private wire descriptor (SSOT) for the GET
// /api/diag/panic response (B1-1188 diag conversion, B1-1054 stream).
// Migration of panic_get_handler's hand-streamed bb_http_resp_json_obj_*
// emitter (platform/espidf/bb_diag_http/bb_diag_http_routes.c) to a
// bb_serialize descriptor rendered via bb_http_serialize_stream(). Locked
// design (do NOT reintroduce a fork): a SINGLE unconditional shape covering
// all 10 fields, gated ONLY by a runtime present-predicate on
// `coredump_avail` -- never a twin descriptor, never a
// `#if CONFIG_BB_DIAG_PANIC_COREDUMP` field-row fork. The coredump
// accessors this route's ESP-IDF gather calls (bb_diag_panic_coredump_*)
// are unconditionally link-safe in every build variant (real / safe-stub /
// host); the runtime predicate on `coredump_avail` alone reproduces the
// pre-migration behavior byte-for-byte in every Kconfig configuration,
// including host (coredump_avail is always false there).
//
// Named "_get_" (not the bare "_panic_wire" the ticket brief suggested) to
// avoid a real type collision: bb_diag_boot_wire.h (components/bb_diag)
// already defines `bb_diag_panic_wire_t` for the "panic" nested object
// inside the UNRELATED "diag.boot" wire shape (available/boots_since
// only). This file's wire struct is the full 10-field GET /api/diag/panic
// route response and needed a distinct name.
//
// Shape (mirrors s_panic_get_responses[]'s hand-authored OpenAPI literal,
// platform/espidf/bb_diag_http/bb_diag_http_routes.c):
//   {"available"[,"boots_since"][,"reset_reason"][,"log_tail"]
//    [,"task"][,"exc_pc"][,"exc_cause"][,"backtrace"]
//    [,"panic_reason"][,"app_sha256"]}
//
// Present conditions (see bb_diag_panic_get_wire.c for the predicate fns).
// `coredump_avail` below is the RAW bb_diag_panic_coredump_available()
// result; `coredump_fields_present` (precomputed by fill(), NOT a ticket-
// literal name but required for byte-fidelity) is true only when a coredump
// summary was ALSO successfully fetched (bb_diag_panic_coredump_get() ==
// BB_OK) -- the pre-migration handler gated boots_since on the RAW
// availability flag but gated every other coredump-derived field on a
// successful get() (see panic_get_handler's nested `if` structure before
// this migration); collapsing both to one flag would change behavior on a
// present-but-unreadable coredump (rare, but a real divergence):
//   available     -- always (schema "required")
//   boots_since   -- available || coredump_avail (RAW)
//   reset_reason  -- available
//   log_tail      -- available && log fetched ok (precomputed by fill())
//   task          -- coredump_fields_present
//   exc_pc        -- coredump_fields_present
//   exc_cause     -- coredump_fields_present
//   backtrace     -- coredump_fields_present (array; empty [] when
//                     bt_count == 0, matching current RISC-V behavior --
//                     not additionally gated on count > 0)
//   panic_reason  -- coredump_fields_present && summary.panic_reason[0] != '\0'
//                     (precomputed by fill())
//   app_sha256    -- coredump_fields_present && summary.app_sha256[0] != '\0'
//                     (precomputed by fill())
//
// bb_diag_panic_get_wire_fill() is a pure populate helper -- no ESP-IDF
// symbols -- that takes the already-gathered raw values (the ESP-IDF-only
// gather stays in panic_get_handler) and produces a host-testable
// snapshot. Portable: compiles on host + ESP-IDF.
//
// Included by:
//   - platform/espidf/bb_diag_http/bb_diag_http_routes.c (the live handler)
//   - test/test_host/test_bb_diag_panic_get_wire.c (expected-JSON fixtures)
//   - test/test_host/test_bb_diag_panic_get_wire_meta_golden.c (meta golden)

#include "bb_diag.h"
#include "bb_serialize.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wire snapshot: flat object, all 10 fields unconditionally present in the
// struct (present/absent on the wire is gated at emit time by the
// descriptor's `.present` predicates, never by this struct's shape).
typedef struct {
    bool    available;
    int64_t boots_since;
    char    reset_reason[16];
    char    log_tail[512];
    char    task[BB_DIAG_PANIC_TASK_NAME_MAX];
    int64_t exc_pc;
    int64_t exc_cause;

    // "backtrace": BB_TYPE_ARR of scalar BB_TYPE_I64, backed by a fixed
    // materialized buffer -- bb_serialize_walk()'s scalar-array branch
    // (B1-1032) reads a contiguous typed value buffer via `.items`/`.count`.
    int64_t             backtrace_items[BB_DIAG_PANIC_BACKTRACE_MAX];
    bb_serialize_arr_t  backtrace;

    char    panic_reason[BB_DIAG_PANIC_REASON_MAX];
    char    app_sha256[BB_DIAG_PANIC_APP_SHA256_MAX];

    // Precomputed present-flags -- the walker's `.present` predicate can
    // only inspect the snapshot, it cannot call bb_diag_panic_get() or
    // evaluate a 2-condition string check itself, so fill() resolves these
    // ahead of the walk.
    bool coredump_avail;          // RAW bb_diag_panic_coredump_available() -- gates boots_since only
    bool coredump_fields_present; // coredump_avail && the coredump summary was fetched OK -- gates
                                   // task/exc_pc/exc_cause/backtrace
    bool log_tail_present;
    bool panic_reason_present;
    bool app_sha256_present;
} bb_diag_panic_get_wire_t;

_Static_assert(sizeof(((bb_diag_panic_get_wire_t *)0)->boots_since) == 8,
               "bb_diag_panic_get_wire_t.boots_since must be exactly 8 bytes for BB_TYPE_I64");
_Static_assert(sizeof(((bb_diag_panic_get_wire_t *)0)->exc_pc) == 8,
               "bb_diag_panic_get_wire_t.exc_pc must be exactly 8 bytes for BB_TYPE_I64");
_Static_assert(sizeof(((bb_diag_panic_get_wire_t *)0)->exc_cause) == 8,
               "bb_diag_panic_get_wire_t.exc_cause must be exactly 8 bytes for BB_TYPE_I64");

// Top-level object descriptor: renders the shape documented above via
// bb_http_serialize_stream()/bb_serialize_json_render() -- byte-identical
// to the pre-migration hand cJSON emitter (panic_get_handler).
extern const bb_serialize_desc_t bb_diag_panic_get_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-3a meta-derivation
// feeder) -- co-located JSON Schema docs/validation table for
// bb_diag_panic_get_wire_desc above, same #if-gated pattern as
// bb_diag_heap_check_wire_priv.h's exemplar. BB_SERIALIZE_META_HOST is a
// host-only define (set by the PlatformIO native env; see platformio.ini)
// -- NEVER set by the ESP-IDF/device build, so this declaration (and its
// definition in bb_diag_panic_get_wire.c) compiles to nothing on-device.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_diag_panic_get_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// GET /api/diag/panic RESPONSE schema (B1-1059 emit batch C, site C1). A
// #define (not just the accessor below) so BOTH this component's config-OFF
// bb_diag_panic_get_wire_get_schema() (wire.c) AND platform/espidf/
// bb_diag_http/bb_diag_http_routes.c's static const s_panic_get_responses[]
// table (cross-TU, ESP-IDF-only -- can't call a function from a static
// initializer) can use the SAME literal text as a genuine compile-time
// constant expression -- mirrors bb_wifi_http_scan_wire_priv.h's
// BB_WIFI_HTTP_SCAN_SCHEMA_LITERAL precedent (B1-1059 emit batch B, site B2).
// ---------------------------------------------------------------------------
#define BB_DIAG_PANIC_GET_SCHEMA_LITERAL \
    "{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"available\":{\"type\":\"boolean\"}," \
    "\"boots_since\":{\"type\":\"integer\"}," \
    "\"reset_reason\":{\"type\":\"string\"}," \
    "\"log_tail\":{\"type\":\"string\"}," \
    "\"task\":{\"type\":\"string\"}," \
    "\"exc_pc\":{\"type\":\"integer\"}," \
    "\"exc_cause\":{\"type\":\"integer\"}," \
    "\"backtrace\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}," \
    "\"panic_reason\":{\"type\":\"string\"}," \
    "\"app_sha256\":{\"type\":\"string\"}}," \
    "\"required\":[\"available\"]}"

// Composed-schema accessor pair (B1-1059 emit batch C, site C1) -- cross-TU
// bridge: the register site (platform/espidf/bb_diag_http/
// bb_diag_http_routes.c) is ESP-IDF-only and cannot host a compose buffer or
// call the meta engine directly on host, so this portable TU owns the
// buffer/composer and exposes two accessors -- mirrors
// bb_wifi_http_scan_wire_priv.h's site-B2 exemplar.
// bb_diag_panic_get_wire_get_schema() is ALWAYS declared (zero config-OFF
// footprint beyond the accessor itself: it returns the pre-existing
// BB_DIAG_PANIC_GET_SCHEMA_LITERAL, unchanged);
// bb_diag_panic_get_wire_ensure_schema_patched() exists ONLY under
// CONFIG_BB_OPENAPI_RUNTIME_META.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_diag_panic_get_wire_ensure_schema_patched(void);
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_diag_panic_get_wire_get_schema(void);

// Pure populate helper: zero-inits `dst`, widens/copies the already-
// gathered scalar values, materializes the backtrace array from `summary`,
// and computes every precomputed present-flag (including
// `coredump_fields_present`, derived here as `coredump_avail && summary !=
// NULL`). No ESP-IDF symbols -- host-testable without a live panic/coredump.
//
// `coredump_avail` is the RAW bb_diag_panic_coredump_available() result --
// gates ONLY `boots_since` (mirrors the pre-migration handler's
// `available || coredump_avail` boots_since guard). `summary` must be
// passed NULL whenever the caller did not also successfully fetch a
// coredump summary (bb_diag_panic_coredump_get() != BB_OK) -- passing a
// non-NULL `summary` is what drives task/exc_pc/exc_cause/backtrace/
// panic_reason/app_sha256 presence, independent of `coredump_avail` alone.
void bb_diag_panic_get_wire_fill(bb_diag_panic_get_wire_t *dst,
                                  bool available, bool coredump_avail,
                                  uint32_t boots_since, const char *reset_reason,
                                  bool log_tail_ok, const char *log_tail,
                                  const bb_diag_panic_summary_t *summary);

#ifdef __cplusplus
}
#endif
