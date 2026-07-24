#pragma once

// bb_wifi_http_creds_wire — private wire descriptor (SSOT) for the PATCH
// /api/wifi credentials-apply request (B1-1022's "wifi" bb_data ingress
// binding). Extracted (B1-1178) out of
// platform/espidf/bb_wifi_http/bb_wifi_http_routes.c -- that TU is
// ESP-IDF-only (includes <esp_wifi.h> unconditionally, its component
// PRIV_REQUIRES esp_wifi) and cannot link on host, so its request
// descriptor could not host a BB_SERIALIZE_META_HOST meta table there, and
// test_wifi_creds_apply_route.c had to hand-duplicate the descriptor
// (s_mirror_desc/mirror_creds_t) to drive bb_data_apply() on host. This
// descriptor + its wire type are portable (no ESP-IDF/FreeRTOS types) --
// mirrors bb_wifi_http_wire_priv.h's GET-side precedent (B1-1059 PR-2a).
//
// Included by:
//   - platform/espidf/bb_wifi_http/bb_wifi_http_routes.c (the live PATCH
//     /api/wifi bb_data binding, CONFIG_BB_WIFI_RECONFIGURE-gated)
//   - test/test_host/test_wifi_creds_apply_route.c (drives the real
//     descriptor through bb_data_apply(), no more hand-duplicated mirror)
//   - test/test_host/test_bb_wifi_http_creds_wire_meta_golden.c
//     (BB_SERIALIZE_META_HOST golden)

#include "bb_serialize.h"
#include "bb_wifi_pending.h"

#ifdef __cplusplus
extern "C" {
#endif

// Buffer sizing (Fork 1, B1-1022, user-decided): bb_serialize_populate()'s
// get_str SILENTLY TRUNCATES a value that overflows a field's max_len --
// the PATCH /api/wifi route instead REJECTS an oversized ssid/password with
// a 400. To preserve that reject-UX without changing populate's shipped
// truncate contract, ssid/pass here are given MORE buffer than the real
// BB_WIFI_PENDING_SSID_MAX/PASS_MAX limits, so an oversized value survives
// intact (or, past this buffer's own cap, is detectably "buffer-full")
// rather than being silently clipped down to exactly the real limit --
// wifi_creds_apply() (bb_wifi_http_routes.c) then re-checks against the
// real limit via bb_wifi_pending_validate_buf() and rejects. See that fn's
// own doc for the full truncation-safety argument.
#define BB_WIFI_HTTP_CREDS_WIRE_SSID_BUF (BB_WIFI_PENDING_SSID_MAX + 1 + 32)  // 64
#define BB_WIFI_HTTP_CREDS_WIRE_PASS_BUF (BB_WIFI_PENDING_PASS_MAX + 1 + 32)  // 96

typedef struct {
    char ssid[BB_WIFI_HTTP_CREDS_WIRE_SSID_BUF];
    char pass[BB_WIFI_HTTP_CREDS_WIRE_PASS_BUF];
} bb_wifi_http_creds_wire_t;

extern const bb_serialize_desc_t bb_wifi_http_creds_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1178) -- co-located JSON Schema
// docs/validation table for bb_wifi_http_creds_wire_desc above, same
// #if-gated pattern as bb_wifi_http_wire_priv.h's GET-side exemplar
// (B1-1059 PR-2a). BB_SERIALIZE_META_HOST is a host-only define (set by the
// PlatformIO native env; see platformio.ini) -- NEVER set by the ESP-IDF/
// device build, so this declaration (and its definition in
// bb_wifi_http_creds_wire.c) compiles to nothing on-device.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_wifi_http_creds_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

#ifdef __cplusplus
}
#endif
