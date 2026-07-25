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
// nothing on-device unless CONFIG_BB_OPENAPI_RUNTIME_META is also on.
// bb_serialize_meta.h now lives under components/bb_serialize_meta/include/
// (B1-1059 relocated it out of its original host-only host_tools/
// bb_serialize_meta/ location so the engine could ship on-device, gated
// off by default via CONFIG_BB_OPENAPI_RUNTIME_META) -- this component
// REQUIRES bb_serialize_meta unconditionally (CMakeLists.txt), so its
// include path is always available and the #include below is therefore
// unconditional too; only the extern declaration right after it is
// #if-gated on BB_SERIALIZE_META_SHIP.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_wifi_http_info_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// Composed-schema accessor pair (B1-1059 emit batch B, site B1) -- cross-TU
// bridge: the register site (platform/espidf/bb_wifi_http/
// bb_wifi_http_routes.c) is ESP-IDF-only (includes <esp_wifi.h>
// unconditionally) and cannot host a compose buffer or call the meta engine
// directly on host, so this portable TU owns the buffer/composer and
// exposes two accessors -- mirrors bb_health_stack_wire.h's precedent.
// bb_wifi_http_info_wire_get_schema() is ALWAYS declared (zero config-OFF
// footprint beyond the accessor itself: it returns the pre-existing
// k_wifi_info_schema literal, relocated here verbatim from
// bb_wifi_http_routes.c); bb_wifi_http_info_wire_ensure_schema_patched()
// exists ONLY under CONFIG_BB_OPENAPI_RUNTIME_META.
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_wifi_http_info_wire_ensure_schema_patched(void);
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_wifi_http_info_wire_get_schema(void);

// Fills `dst` from `info` plus the two global recovery-telemetry getters
// (bb_wifi_get_restart_sta_count/bb_wifi_get_disconnect_rssi) — same
// sources bb_wifi_emit_section read. Pure/portable; `dst` is NOT
// zero-initialized by this fn (every field is written unconditionally), so
// callers pass an already-declared (uninitialized is fine) snapshot.
void bb_wifi_http_info_wire_fill(bb_wifi_http_info_wire_t *dst, const bb_wifi_info_t *info);

#ifdef __cplusplus
}
#endif
