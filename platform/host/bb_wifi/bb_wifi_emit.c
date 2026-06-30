// bb_wifi_emit — shared per-datum wifi section emitter.
// Compiled on both host (tests/bb_pub_wifi) and ESP-IDF (bb_wifi routes,
// bb_info, bb_health).  This is the single source of truth for the wifi
// section wire format.
//
// When disconnected all numeric fields are 0/false and string fields are
// empty/"0.0.0.0", matching the current /api/wifi behaviour.
#include "bb_wifi.h"
#include "bb_json.h"

#include <stdio.h>
#include <string.h>

// Emit the canonical wifi section into obj.
// Fields written (in order):
//   ssid               (string)
//   bssid              (string "aa:bb:cc:dd:ee:ff")
//   rssi               (integer)
//   ip                 (string)
//   connected          (bool)
//   disc_reason        (integer)
//   disc_age_s         (integer)
//   retry_count        (integer)
//   no_ip_recoveries   (integer, diagnostics — times no-IP watchdog triggered)
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
    bb_json_obj_set_int   (obj, "disc_reason",      (int64_t)info->disc_reason);
    bb_json_obj_set_int   (obj, "disc_age_s",       (int64_t)info->disc_age_s);
    bb_json_obj_set_int   (obj, "retry_count",      (int64_t)info->retry_count);
    bb_json_obj_set_int   (obj, "no_ip_recoveries",   (int64_t)bb_wifi_get_no_ip_count());
    bb_json_obj_set_int   (obj, "egress_dead_count",  (int64_t)bb_wifi_get_egress_dead_count());
    bb_json_obj_set_int   (obj, "lost_ip_count",      (int64_t)bb_wifi_get_lost_ip_count());
    bb_json_obj_set_int   (obj, "recovery_count",
                           (int64_t)(bb_wifi_get_no_ip_count() +
                                     bb_wifi_get_egress_dead_count() +
                                     bb_wifi_get_lost_ip_count()));
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
