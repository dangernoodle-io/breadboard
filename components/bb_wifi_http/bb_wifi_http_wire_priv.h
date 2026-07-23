#pragma once

// bb_wifi_http_wire — private wire descriptor (SSOT) for the GET /api/wifi
// response (B1-1057: migration of bb_wifi_emit_section, the bb_json_t-based
// emitter this replaces, to a bb_serialize_desc_t). Field order/names/types/
// null-omit semantics are byte-identical to the emitter it replaces — see
// test/test_host/test_v2_golden.c's wifi goldens for the byte-fidelity
// proof. Portable: no ESP-IDF/FreeRTOS types, compiles on host + ESP-IDF
// (mirrors components/bb_health/bb_health_wire_priv.h's pattern).
//
// Included by:
//   - platform/espidf/bb_wifi_http/bb_wifi_http_routes.c (the live handler)
//   - test/test_host/test_v2_golden.c (byte-fidelity goldens)
//   - test/test_host/test_route_fidelity.c (schema-conformance fixture,
//     driven through bb_wifi_http_info_wire_fill() -- the real production
//     fill fn, not a hand-copy)

#include "bb_serialize.h"
#include "bb_wifi.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GET /api/wifi response shape. Field order matches bb_wifi_emit_section's
// former emit order: ssid, bssid, rssi, ip, connected, disc_reason,
// disc_age_s, retry_count, restart_sta_count, disconnect_rssi.
typedef struct {
    char                  ssid[33];
    char                  bssid[18];  // "aa:bb:cc:dd:ee:ff"
    int64_t               rssi;
    char                  ip[16];
    bool                  connected;
    bb_serialize_str_n_t  disc_reason;  // stable label, see bb_wifi_disc_reason_str
    int64_t               disc_age_s;
    int64_t               retry_count;
    int64_t               restart_sta_count;
    int64_t               disconnect_rssi;
} bb_wifi_http_info_wire_t;

extern const bb_serialize_desc_t bb_wifi_http_info_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-2a) -- co-located JSON
// Schema docs/validation table for bb_wifi_http_info_wire_desc above,
// exemplar for the #if-gated meta-table pattern. BB_SERIALIZE_META_HOST is
// a host-only define (set by the PlatformIO native env; see
// platformio.ini) -- NEVER set by the ESP-IDF/device build, so this
// declaration (and its definition in bb_wifi_http_wire.c) compiles to
// nothing on-device. bb_serialize_meta.h lives under
// host_tools/bb_serialize_meta/ (PR-1), unreachable from any ESP-IDF
// component build; the #include is itself compiled out when the guard is
// off, so no extra include path is needed for the device build.
#if defined(BB_SERIALIZE_META_HOST)
#include "bb_serialize_meta.h"

extern const bb_serialize_desc_meta_t bb_wifi_http_info_wire_meta;
#endif /* BB_SERIALIZE_META_HOST */

// Fills `dst` from `info` plus the two global recovery-telemetry getters
// (bb_wifi_get_restart_sta_count/bb_wifi_get_disconnect_rssi) — same
// sources bb_wifi_emit_section read. Pure/portable; `dst` is NOT
// zero-initialized by this fn (every field is written unconditionally), so
// callers pass an already-declared (uninitialized is fine) snapshot.
void bb_wifi_http_info_wire_fill(bb_wifi_http_info_wire_t *dst, const bb_wifi_info_t *info);

#ifdef __cplusplus
}
#endif
