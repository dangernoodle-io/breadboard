// bb_wifi_http_emit — bb_json_t-based wifi section emitters.
// Compiled on both host (tests) and ESP-IDF (bb_wifi_http routes, bb_health).
//
// PR1 (KB 781): relocated verbatim from platform/host/bb_wifi/bb_wifi_emit.c
// so bb_wifi's public header (and therefore the STA core's dependency
// closure) sheds bb_json. This is the single source of truth for the wifi
// section wire format.
//
// When disconnected all numeric fields are 0/false and string fields are
// empty/"0.0.0.0", matching the current /api/wifi behaviour.
//
// KB 820 (bb_wifi reason contract, PR1): "disc_reason" changed from a raw
// esp_wifi/backend-specific numeric code to a STABLE STRING label (e.g.
// "handshake_timeout", "bb_lost_ip") backed by the portable
// bb_wifi_disc_reason_t enum. This is a BREAKING CHANGE for any consumer
// that parsed "disc_reason" as an integer -- the numeric value is no longer
// wire-visible anywhere. See bb_wifi.h for the full enum + per-backend
// mapping (bb_wifi_map_esp_reason / bb_wifi_map_wl_status).
#include "bb_wifi_http.h"

#include <stdio.h>

// Emit the canonical wifi section into obj.
// Fields written (in order):
//   ssid               (string)
//   bssid              (string "aa:bb:cc:dd:ee:ff")
//   rssi               (integer)
//   ip                 (string)
//   connected          (bool)
//   disc_reason        (string — stable label, see bb_wifi_disc_reason_str)
//   disc_age_s         (integer)
//   retry_count        (integer)
//   restart_sta_count  (integer, times bb_wifi_restart_sta was invoked)
//   disconnect_rssi    (integer, RSSI at most recent disconnect)
void bb_wifi_emit_section(bb_json_t obj, const bb_wifi_info_t *info)
{
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             info->bssid[0], info->bssid[1], info->bssid[2],
             info->bssid[3], info->bssid[4], info->bssid[5]);

    bb_json_obj_set_string(obj, "ssid",             info->ssid);
    bb_json_obj_set_string(obj, "bssid",            bssid);
    bb_json_obj_set_int   (obj, "rssi",             (int64_t)info->rssi);
    bb_json_obj_set_string(obj, "ip",               info->ip);
    bb_json_obj_set_bool  (obj, "connected",        info->connected);
    bb_json_obj_set_string(obj, "disc_reason",      bb_wifi_disc_reason_str(info->disc_reason));
    bb_json_obj_set_int   (obj, "disc_age_s",       (int64_t)info->disc_age_s);
    bb_json_obj_set_int   (obj, "retry_count",      (int64_t)info->retry_count);
    bb_json_obj_set_int   (obj, "restart_sta_count",  (int64_t)bb_wifi_get_restart_sta_count());
    bb_json_obj_set_int   (obj, "disconnect_rssi",    (int64_t)bb_wifi_get_disconnect_rssi());
}

// Emit status-only wifi fields — ssid/bssid/ip/connected.
// No numeric fields; this is the SSOT for the /api/health "network" section (TA-505).
void bb_wifi_emit_status(bb_json_t obj)
{
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             info.bssid[0], info.bssid[1], info.bssid[2],
             info.bssid[3], info.bssid[4], info.bssid[5]);

    bb_json_obj_set_string(obj, "ssid",      info.ssid);
    bb_json_obj_set_string(obj, "bssid",     bssid);
    bb_json_obj_set_string(obj, "ip",        info.ip);
    bb_json_obj_set_bool  (obj, "connected", info.connected);
}
