#pragma once

#include "bb_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// bb_wifi_http — opt-in STA route bundle for bb_wifi (PR1, KB 781).
//
// Registers GET /api/wifi, POST /api/wifi/scan, GET /api/diag/wifi, and — when
// CONFIG_BB_WIFI_RECONFIGURE=y — PATCH /api/wifi on the shared
// bb_http_server. This is a route BUNDLE, not a server: it composes on top
// of bb_wifi (STA core) and the app's own bb_http_server instance.
//
// GET /api/diag/wifi (B1-969) rehomes the WiFi diagnostic surface from the
// dissolved bb_net_health component's GET /api/diag/net, reduced to
// bb_wifi-native fields only (no gw/egress/early_warning/transport-health
// fields -- those were dropped, not migrated).
//
// Extracted so the bb_wifi STA core does not carry bb_http/bb_json/
// bb_openapi in its dependency closure — a minimal STA-only image can omit
// bb_wifi_http entirely and pay none of that heap. See
// components/bb_wifi_http/README.md.

// Format `bssid` as a colon-hex MAC string ("aa:bb:cc:dd:ee:ff") into `out`
// (must be at least 18 bytes). Pure, host-testable -- the shared idiom
// behind bb_health's root "network" gather (platform/espidf/bb_health/
// bb_health.c), the "wifi" bb_diag section's fill (bb_wifi_http_diag.h),
// and the GET /api/wifi wire descriptor (bb_wifi_http_wire_priv.h),
// factored out per the hand-roll-twice-extract convention rather than a
// third snprintf copy.
void bb_wifi_http_format_bssid(char out[18], const uint8_t bssid[6]);

// The GET /api/wifi bb_json_t emitter (bb_wifi_emit_section) was migrated
// to a bb_serialize_desc_t (B1-1057) -- see bb_wifi_http_wire_priv.h
// (bb_wifi_http_info_wire_desc / bb_wifi_http_info_wire_fill), the SSOT
// wifi_info_handler now renders through.
//
// bb_wifi_emit_status (the bb_json_t-based status-only emitter, TA-505) is
// DELETED (B1-1149) -- the /api/health "network" section it backed renders
// through bb_health_wire_desc (components/bb_health/bb_health_wire_priv.h)
// instead, a bb_serialize_desc_t, not a bb_json_t emitter. This header no
// longer includes bb_json.h.

/// Reserve route-table slots for bb_wifi_http before the HTTP server starts.
// bbtool:init tier=pre_http fn=bb_wifi_routes_reserve
bb_err_t bb_wifi_routes_reserve(void);

/// Registry hook — registers GET /api/wifi, POST /api/wifi/scan, GET
/// /api/diag/wifi, and (when CONFIG_BB_WIFI_RECONFIGURE=y) PATCH /api/wifi.
// bbtool:init tier=regular fn=bb_wifi_routes_init server=true
bb_err_t bb_wifi_routes_init(bb_http_handle_t server);

#ifdef __cplusplus
}
#endif
