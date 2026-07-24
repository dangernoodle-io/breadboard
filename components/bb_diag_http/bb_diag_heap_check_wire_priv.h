#pragma once

// bb_diag_heap_check_wire — private wire descriptor (SSOT) for the GET
// /api/diag/heap-check response. Response body is a single top-level object
// with one field: {"integrity_ok":<bool>} — migration of
// heap_check_get_handler's hand-streamed bb_http_resp_json_obj_* emitter
// (platform/espidf/bb_diag_http/bb_diag_http_routes.c) to a bb_serialize
// descriptor rendered via bb_http_serialize_stream(). Deliberately the
// trivial proof-of-pattern conversion in the diag-conversion batch (B1-1054)
// — same object-wrap-with-scalar-field shape as
// bb_storage_http_delete_wire_priv.h's "key" field, minus the array field
// and the present-predicate.
//
// heap_caps_check_integrity_all() itself is NOT called from anywhere in this
// file — it stays in heap_check_get_handler (ESP-IDF-only, no host stub
// exists for it in this repo). bb_diag_heap_check_wire_fill() below is a
// pure populate helper (mirrors bb_storage_http_delete_wire_fill's shape)
// that takes the already-computed bool and copies it into the wire snapshot
// — this keeps the descriptor/fill pair host-testable without a live heap
// walk. Portable: no ESP-IDF/FreeRTOS types, compiles on host + ESP-IDF.
//
// Included by:
//   - platform/espidf/bb_diag_http/bb_diag_http_routes.c (the live handler)
//   - test/test_host/test_bb_diag_heap_check_wire.c (expected-JSON fixtures)
//   - test/test_host/test_bb_diag_heap_check_wire_meta_golden.c (meta golden)

#include "bb_serialize.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wire snapshot: single bool field, no backing storage beyond itself.
typedef struct {
    bool integrity_ok;
} bb_diag_heap_check_wire_t;

// Top-level object descriptor: "integrity_ok" (BB_TYPE_BOOL). Renders
// {"integrity_ok":true|false} via bb_http_serialize_stream()/
// bb_serialize_json_render() — byte-identical to the pre-migration hand
// cJSON emitter.
extern const bb_serialize_desc_t bb_diag_heap_check_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-3a meta-derivation feeder)
// — co-located JSON Schema docs/validation table for
// bb_diag_heap_check_wire_desc above, same #if-gated pattern as
// bb_storage_http_delete_wire_priv.h's exemplar. BB_SERIALIZE_META_HOST is a
// host-only define (set by the PlatformIO native env; see platformio.ini) —
// NEVER set by the ESP-IDF/device build, so this declaration (and its
// definition in bb_diag_heap_check_wire.c) compiles to nothing on-device.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_diag_heap_check_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// GET /api/diag/heap-check RESPONSE schema (B1-1059 emit batch C, site C2).
// A #define (not just the accessor below) so BOTH this component's
// config-OFF bb_diag_heap_check_wire_get_schema() (wire.c) AND
// platform/espidf/bb_diag_http/bb_diag_http_routes.c's static const
// s_heap_check_get_responses[] table (cross-TU, ESP-IDF-only -- can't call
// a function from a static initializer) can use the SAME literal text as a
// genuine compile-time constant expression -- mirrors
// bb_wifi_http_scan_wire_priv.h's BB_WIFI_HTTP_SCAN_SCHEMA_LITERAL
// precedent (B1-1059 emit batch B, site B2).
// ---------------------------------------------------------------------------
#define BB_DIAG_HEAP_CHECK_SCHEMA_LITERAL \
    "{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"integrity_ok\":{\"type\":\"boolean\"}}," \
    "\"required\":[\"integrity_ok\"]}"

// Composed-schema accessor pair (B1-1059 emit batch C, site C2) -- cross-TU
// bridge: the register site (platform/espidf/bb_diag_http/
// bb_diag_http_routes.c) is ESP-IDF-only and cannot host a compose buffer or
// call the meta engine directly on host, so this portable TU owns the
// buffer/composer and exposes two accessors -- mirrors
// bb_wifi_http_scan_wire_priv.h's site-B2 exemplar.
// bb_diag_heap_check_wire_get_schema() is ALWAYS declared (zero config-OFF
// footprint beyond the accessor itself: it returns the pre-existing
// BB_DIAG_HEAP_CHECK_SCHEMA_LITERAL, unchanged);
// bb_diag_heap_check_wire_ensure_schema_patched() exists ONLY under
// CONFIG_BB_OPENAPI_RUNTIME_META.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_diag_heap_check_wire_ensure_schema_patched(void);
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_diag_heap_check_wire_get_schema(void);

// Pure populate helper: zero-inits `dst`, then copies `integrity_ok` into
// it. Host-testable without a live heap_caps_check_integrity_all() walk —
// the sole reason this is factored out of heap_check_get_handler.
void bb_diag_heap_check_wire_fill(bb_diag_heap_check_wire_t *dst, bool integrity_ok);

#ifdef __cplusplus
}
#endif
