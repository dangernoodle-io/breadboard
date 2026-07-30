#pragma once

// bb_wifi_http_patch_status_wire — private wire descriptor (SSOT) for the
// PATCH /api/wifi 202 success response body,
// {"status":"rebooting_to_try_wifi"} (B1-1286: migration of
// wifi_patch_handler's success path off the bb_http_json_obj_stream_t idiom
// onto bb_http_serialize_stream()). Extracted (firmware-review follow-up,
// same PR) out of platform/espidf/bb_wifi_http/bb_wifi_http_routes.c -- that
// TU is ESP-IDF-only (includes <esp_wifi.h> unconditionally, its component
// PRIV_REQUIRES esp_wifi) and cannot link on host, so a host fidelity test
// could not drive the real production descriptor and had to hand-copy this
// wire shape instead. This descriptor + its wire type are portable (no
// ESP-IDF/FreeRTOS types) -- mirrors bb_wifi_http_creds_wire_priv.h's
// request-side precedent (B1-1178).
//
// Included by:
//   - platform/espidf/bb_wifi_http/bb_wifi_http_routes.c (the live PATCH
//     /api/wifi 202 response, CONFIG_BB_WIFI_RECONFIGURE-gated)
//   - test/test_host/test_route_fidelity.c (drives the real descriptor
//     through bb_http_serialize_stream(), no more hand-copied mirror)

#include "bb_serialize.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char status[32];
} bb_wifi_http_patch_status_wire_t;

extern const bb_serialize_desc_t bb_wifi_http_patch_status_wire_desc;

#ifdef __cplusplus
}
#endif
