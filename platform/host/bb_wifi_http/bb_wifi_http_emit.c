// bb_wifi_http_emit — bb_json-free wifi helpers.
// Compiled on both host (tests) and ESP-IDF (bb_wifi_http routes, bb_health).
//
// PR1 (KB 781): relocated verbatim from platform/host/bb_wifi/bb_wifi_emit.c
// so bb_wifi's public header (and therefore the STA core's dependency
// closure) sheds bb_json. This file's SOLE remaining symbol,
// bb_wifi_http_format_bssid, is the shared colon-hex formatter behind the
// wifi status-only wire shape (ssid/bssid/ip/connected) used by both
// bb_health's root "network" section (platform/espidf/bb_health/
// bb_health.c) and GET /api/wifi's own wire descriptor
// (bb_wifi_http_wire_priv.h).
//
// B1-1057: the full GET /api/wifi emitter (bb_wifi_emit_section) migrated to
// a bb_serialize_desc_t -- see bb_wifi_http_wire_priv.h
// (bb_wifi_http_info_wire_desc / bb_wifi_http_info_wire_fill).
//
// B1-1149: this file's OWN bb_json_t emitter, bb_wifi_emit_status, is
// DELETED -- its sole caller was a host test fixture (test_route_fidelity.c
// h_health), never any production route (bb_health.c's health_handler
// gathers ssid/bssid/ip/connected directly into bb_health_wire_t and renders
// via bb_health_wire_desc, a bb_serialize_desc_t -- it never called this
// function). The test fixture now renders through bb_health_wire_desc
// directly too, so no bb_json-based mirror of this shape remains anywhere in
// the tree; keeping bb_wifi_emit_status around would have been a second,
// unused implementation of a shape bb_health_wire_desc already owns.
#include "bb_wifi_http.h"

#include <stdio.h>

// Shared bssid colon-hex formatter -- see bb_wifi_http.h's doc comment.
void bb_wifi_http_format_bssid(char out[18], const uint8_t bssid[6])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}
