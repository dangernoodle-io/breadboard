#pragma once

#include "bb_wifi.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// bb_wifi_http — opt-in STA route bundle for bb_wifi (PR1, KB 781).
//
// Registers GET /api/wifi, POST /api/scan, GET /api/diag/wifi, and — when
// CONFIG_BB_WIFI_RECONFIGURE=y — PATCH /api/wifi on the shared
// bb_http_server. This is a route BUNDLE, not a server: it composes on top
// of bb_wifi (STA core) and the app's own bb_http_server instance.
//
// GET /api/diag/wifi (B1-969) rehomes the WiFi diagnostic surface from the
// dissolved bb_net_health component's GET /api/diag/net, reduced to
// bb_wifi-native fields only (no gw/egress/early_warning/
// bb_transport_health -- those were dropped, not migrated).
//
// Extracted so the bb_wifi STA core does not carry bb_http/bb_json/
// bb_openapi in its dependency closure — a minimal STA-only image can omit
// bb_wifi_http entirely and pay none of that heap. See
// components/bb_wifi_http/README.md.

// Emit the canonical wifi section into a bb_json_t object.
// Writes: ssid, bssid (colon-hex), rssi (integer), ip, connected,
// disc_reason (integer), disc_age_s (integer), retry_count (integer).
// When disconnected all numeric fields are 0/false, strings empty/"0.0.0.0".
void bb_wifi_emit_section(bb_json_t obj, const bb_wifi_info_t *info);

// Emit status-only wifi fields into a bb_json_t object (TA-505).
// Writes: ssid, bssid (colon-hex), ip, connected — no numeric fields.
// Calls bb_wifi_get_info internally; no info parameter required.
void bb_wifi_emit_status(bb_json_t obj);

/// Reserve route-table slots for bb_wifi_http before the HTTP server starts.
// bbtool:init tier=pre_http fn=bb_wifi_routes_reserve
bb_err_t bb_wifi_routes_reserve(void);

/// Registry hook — registers GET /api/wifi, POST /api/scan, GET
/// /api/diag/wifi, and (when CONFIG_BB_WIFI_RECONFIGURE=y) PATCH /api/wifi.
// bbtool:init tier=regular fn=bb_wifi_routes_init server=true
bb_err_t bb_wifi_routes_init(bb_http_handle_t server);

#ifdef __cplusplus
}
#endif
