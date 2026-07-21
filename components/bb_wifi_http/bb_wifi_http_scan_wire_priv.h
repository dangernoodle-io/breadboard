#pragma once

// bb_wifi_http_scan_wire — private wire descriptor (SSOT) for the POST
// /api/wifi/scan response. Response body is a top-level object with one
// array field: {"aps":[{ssid,rssi,secure}, ...]} -- migration of
// scan_handler's bb_json_t-based emitter to a bb_serialize descriptor,
// mirroring bb_diag_storage_partitions.h's BB_TYPE_ARR + BB_ARR_FIXED
// (elem_type == BB_TYPE_OBJ) wiring. Portable: no ESP-IDF/FreeRTOS types,
// compiles on host + ESP-IDF (mirrors this component's own
// bb_wifi_http_wire_priv.h pattern).
//
// Included by:
//   - platform/espidf/bb_wifi_http/bb_wifi_http_routes.c (the live handler)
//   - test/test_host/test_wifi_scan_wire.c (expected-JSON fixtures)

#include "bb_serialize.h"
#include "bb_wifi.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One access-point row in the POST /api/wifi/scan response's "aps" array.
// rssi is widened to int64_t for BB_TYPE_I64 -- same convention as
// bb_wifi_http_wire_priv.h's own rssi field -- so the rendered numeric text
// is identical to the bb_wifi_ap_t.rssi (int8_t) source it's copied from.
typedef struct {
    char    ssid[33];
    int64_t rssi;
    bool    secure;
} bb_wifi_http_scan_ap_wire_t;

// Row descriptor (3 fields) -- shared by the production handler and the
// host tests.
extern const bb_serialize_field_t bb_wifi_http_scan_ap_wire_fields[3];
// SSOT field count, computed from the array above -- mirrors
// bb_partition_row_n_fields's pattern. Callers pass this, never a hand-typed
// literal, so the count can never desync from the array.
extern const uint16_t             bb_wifi_http_scan_ap_wire_n_fields;

// Top-level object snapshot: `aps_items` is the backing row storage,
// `aps` is the bb_serialize_arr_t carrier the descriptor's "aps" BB_TYPE_ARR
// field points at -- same storage/carrier split as
// bb_diag_storage_partitions_snap_t's rows_items/rows.
typedef struct {
    bb_wifi_http_scan_ap_wire_t aps_items[WIFI_SCAN_MAX];
    bb_serialize_arr_t          aps;
} bb_wifi_http_scan_wire_t;

// Top-level object descriptor: one field, key "aps", BB_TYPE_ARR of
// BB_TYPE_OBJ rows (bb_wifi_http_scan_ap_wire_fields), BB_ARR_FIXED
// (default) cardinality. Renders {"aps":[...]} via
// bb_http_serialize_stream()/bb_serialize_json_render(), same entry points
// every other object-shaped route in this component uses.
extern const bb_serialize_desc_t bb_wifi_http_scan_wire_desc;

// Pure row-copy helper: copies `n` rows from `src` (a live scan result
// array) into `dst` (row-count bounded by WIFI_SCAN_MAX by the caller).
// Host-testable without a live scan -- the sole reason this is factored out
// of bb_wifi_http_scan_wire_fill() below. NUL-safe (strncpy + explicit
// terminate); `n` MUST be <= WIFI_SCAN_MAX -- the caller (both
// bb_wifi_http_scan_wire_fill() and the host tests) is the only source of
// that bound, this helper does not itself clamp it.
void bb_wifi_http_scan_wire_copy_rows(bb_wifi_http_scan_ap_wire_t *dst,
                                       const bb_wifi_ap_t *src, size_t n);

// Pure clamp helper: bounds a raw bb_wifi_scan_get_cached() return value into
// [0, WIFI_SCAN_MAX] before it is narrowed to size_t in
// bb_wifi_http_scan_wire_fill() below -- guards a negative return (which
// would otherwise wrap to SIZE_MAX on the (size_t) cast and overrun the copy)
// and an over-cap return (defensive; the cached call is passed WIFI_SCAN_MAX
// so a conforming backend never exceeds it). Host-testable in isolation,
// same rationale as bb_wifi_http_scan_wire_copy_rows() above.
size_t bb_wifi_http_scan_wire_clamp_count(int count);

// Fills `dst` from a live scan: zero-inits `dst` (mirrors
// bb_diag_storage_partitions_fill()'s precedent), starts an async scan,
// reads back the cached results (up to WIFI_SCAN_MAX), clamps the returned
// count via bb_wifi_http_scan_wire_clamp_count() above, copies them via
// bb_wifi_http_scan_wire_copy_rows() above, and wires `dst->aps` (items/
// count) to point at the freshly-filled `dst->aps_items`.
void bb_wifi_http_scan_wire_fill(bb_wifi_http_scan_wire_t *dst);

#ifdef __cplusplus
}
#endif
