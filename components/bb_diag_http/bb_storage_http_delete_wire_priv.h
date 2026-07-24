#pragma once

// bb_storage_http_delete_wire — private wire descriptor (SSOT) for the
// DELETE /api/diag/storage response. Response body is a top-level object
// with one array field plus one optional string field:
// {"deleted":[<namespace strings>],"key":<optional string>} — migration of
// storage_delete_handler's incremental cJSON-array-append emitter (built
// during the erase loop by appending to a live bb_json_arr_new() array, then
// wrapped in a bb_json_obj_new() response object after the request's parsed
// cJSON tree is freed) to a bb_serialize descriptor rendered via
// bb_http_serialize_stream(). Mirrors bb_ota_validator_partitions_wire_priv.h's
// object-wrap-with-array-field precedent; the array element type here is
// BB_TYPE_STR (bare namespace-name strings) rather than BB_TYPE_OBJ rows.
//
// "key" is present only when the request named a single key (has_key) --
// same omitted-when-absent contract the old bb_json_obj_set_string() call
// had (it was only called inside `if (has_key)`), reproduced here via a
// present-predicate rather than a distinct field type.
//
// Request PARSING (B1-1147) is now ALSO on bb_serialize -- see the
// "storage_delete" bb_data binding in bb_storage_http_routes.c -- this
// descriptor itself stays emit-side (response) only; the two are unrelated
// bb_serialize descriptors. Portable: no ESP-IDF/FreeRTOS types, compiles
// on host + ESP-IDF.
//
// Included by:
//   - platform/espidf/bb_diag_http/bb_storage_http_routes.c (the live
//     handler; rehomed from platform/espidf/bb_storage_http/ -- B1-1154,
//     bb_storage_http dissolved into bb_diag_http, KB 1477)
//   - test/test_host/test_bb_storage_http_delete_wire.c (expected-JSON
//     fixtures)

#include "bb_serialize.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Max namespaces a single DELETE request may name via an array "namespace"
// value. The array-namespace erase loop was previously uncapped (bounded
// only by the request body's own 1024-byte cap); this bound is a NEW,
// deliberate fail-closed validation introduced by this migration -- a
// request naming more than this many namespaces is rejected 400 BEFORE any
// erase is performed (see storage_delete_handler's bound-check).
#define BB_STORAGE_HTTP_DELETE_NS_MAX 16

// Namespace-name buffer bound -- matches bb_storage_entry_t.ns_or_dir[16]
// (bb_storage.h), the real NVS namespace-name limit (15 chars + NUL) every
// other backend reuses for uniformity.
#define BB_STORAGE_HTTP_DELETE_NS_LEN 16

// Wire snapshot: `deleted_names` is the backing storage for the erased
// namespace strings (copied in during the handler's erase loop, since the
// live parse tree they originally came from is freed before this snapshot
// is built); `deleted_items` is an array of borrowed pointers into
// `deleted_names`, and `deleted` is the bb_serialize_arr_str_t carrier the
// descriptor's "deleted" BB_TYPE_ARR field points at -- same
// storage/items-pointer-array/carrier split as
// bb_ota_validator_partitions_wire_t's partitions_items/partitions, adapted
// for a string (rather than object-row) array element type.
typedef struct {
    char        key[BB_STORAGE_HTTP_DELETE_NS_LEN];
    bool        has_key;
    char        deleted_names[BB_STORAGE_HTTP_DELETE_NS_MAX][BB_STORAGE_HTTP_DELETE_NS_LEN];
    const char *deleted_items[BB_STORAGE_HTTP_DELETE_NS_MAX];
    bb_serialize_arr_str_t deleted;
} bb_storage_http_delete_wire_t;

// Top-level object descriptor: "deleted" (BB_TYPE_ARR of BB_TYPE_STR) plus
// "key" (BB_TYPE_STR, present-gated on has_key). Renders
// {"deleted":[...],"key":...} via bb_http_serialize_stream()/
// bb_serialize_json_render() -- "key" is omitted entirely when has_key is
// false.
extern const bb_serialize_desc_t bb_storage_http_delete_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-2b-i-1) -- co-located JSON
// Schema docs/validation table for bb_storage_http_delete_wire_desc above,
// same #if-gated pattern as bb_wifi_http_wire_priv.h's exemplar (B1-1059
// PR-2a). BB_SERIALIZE_META_HOST is a host-only define (set by the
// PlatformIO native env; see platformio.ini) -- NEVER set by the ESP-IDF/
// device build, so this declaration (and its definition in
// bb_storage_http_delete_wire.c) compiles to nothing on-device.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_storage_http_delete_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage 200 RESPONSE schema (B1-1059 emit batch C, site
// C5). Distinct from bb_storage_http_routes.c's
// BB_STORAGE_HTTP_DELETE_REQUEST_SCHEMA_LITERAL (B1-1059 emit batch A, site
// 1 -- the request body schema, already merged) -- this is the 200 response
// body. A #define (not just the accessor below) so BOTH this component's
// config-OFF bb_storage_http_delete_wire_get_schema() (wire.c) AND
// platform/espidf/bb_diag_http/bb_storage_http_routes.c's static const
// s_storage_delete_responses[] table (cross-TU, ESP-IDF-only -- can't call
// a function from a static initializer) can use the SAME literal text as a
// genuine compile-time constant expression -- mirrors
// bb_wifi_http_scan_wire_priv.h's BB_WIFI_HTTP_SCAN_SCHEMA_LITERAL
// precedent (B1-1059 emit batch B, site B2). Only the 200 response is a
// runtime-compose candidate -- the 400/412/500/501 responses stay plain
// literals in routes.c, unaffected by this migration.
// ---------------------------------------------------------------------------
#define BB_STORAGE_HTTP_DELETE_RESPONSE_SCHEMA_LITERAL \
    "{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"deleted\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}," \
    "\"key\":{\"type\":\"string\"}}," \
    "\"required\":[\"deleted\"]}"

// Composed-schema accessor pair (B1-1059 emit batch C, site C5) -- cross-TU
// bridge: the register site (platform/espidf/bb_diag_http/
// bb_storage_http_routes.c) is ESP-IDF-only and cannot host a compose
// buffer or call the meta engine directly on host, so this portable TU
// owns the buffer/composer and exposes two accessors -- mirrors
// bb_wifi_http_scan_wire_priv.h's site-B2 exemplar.
// bb_storage_http_delete_wire_get_schema() is ALWAYS declared (zero
// config-OFF footprint beyond the accessor itself: it returns the
// pre-existing BB_STORAGE_HTTP_DELETE_RESPONSE_SCHEMA_LITERAL, unchanged);
// bb_storage_http_delete_wire_ensure_schema_patched() exists ONLY under
// CONFIG_BB_OPENAPI_RUNTIME_META.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_storage_http_delete_wire_ensure_schema_patched(void);
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_storage_http_delete_wire_get_schema(void);

// Pure populate helper: zero-inits `dst`, copies `n` namespace names from
// `names` into `dst->deleted_names` (bounded strlcpy-style, explicit
// terminate), wires `dst->deleted_items`/`dst->deleted` to point at that
// backing storage, and sets `dst->key`/`dst->has_key` from `key`/`has_key`.
// Host-testable without a live HTTP request or bb_storage backend -- the
// sole reason this is factored out of storage_delete_handler(). `n` MUST be
// <= BB_STORAGE_HTTP_DELETE_NS_MAX -- the caller (both the handler and the
// host tests) is the only source of that bound, this helper does not itself
// clamp it.
void bb_storage_http_delete_wire_fill(bb_storage_http_delete_wire_t *dst,
                                       const char names[][BB_STORAGE_HTTP_DELETE_NS_LEN], size_t n,
                                       const char *key, bool has_key);

#ifdef __cplusplus
}
#endif
