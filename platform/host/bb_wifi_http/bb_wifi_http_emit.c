// bb_wifi_http_emit — bb_json_t-based wifi section emitters.
// Compiled on both host (tests) and ESP-IDF (bb_wifi_http routes, bb_health).
//
// PR1 (KB 781): relocated verbatim from platform/host/bb_wifi/bb_wifi_emit.c
// so bb_wifi's public header (and therefore the STA core's dependency
// closure) sheds bb_json. This is the single source of truth for the wifi
// STATUS-ONLY (ssid/bssid/ip/connected) wire format.
//
// B1-1057: the full GET /api/wifi emitter (bb_wifi_emit_section, the
// numeric-fields superset of bb_wifi_emit_status below) migrated to a
// bb_serialize_desc_t -- see bb_wifi_http_wire_priv.h
// (bb_wifi_http_info_wire_desc / bb_wifi_http_info_wire_fill).
#include "bb_wifi_http.h"

#include <stdio.h>

// Shared bssid colon-hex formatter -- see bb_wifi_http.h's doc comment.
void bb_wifi_http_format_bssid(char out[18], const uint8_t bssid[6])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

// Emit status-only wifi fields — ssid/bssid/ip/connected.
// No numeric fields; this is the SSOT for the /api/health "network" section (TA-505).
void bb_wifi_emit_status(bb_json_t obj)
{
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    char bssid[18];
    bb_wifi_http_format_bssid(bssid, info.bssid);

    bb_json_obj_set_string(obj, "ssid",      info.ssid);
    bb_json_obj_set_string(obj, "bssid",     bssid);
    bb_json_obj_set_string(obj, "ip",        info.ip);
    bb_json_obj_set_bool  (obj, "connected", info.connected);
}
