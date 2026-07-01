// bb_wifi_emit — shared per-datum wifi section emitter.
// Compiled on both host (tests/bb_pub_wifi) and ESP-IDF (bb_wifi routes,
// bb_info, bb_health).  This is the single source of truth for the wifi
// section wire format.
//
// When disconnected all numeric fields are 0/false and string fields are
// empty/"0.0.0.0", matching the current /api/wifi behaviour.
#include "bb_wifi.h"
#include "bb_json.h"
#include "wifi_hist_priv.h"

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
//   egress_dead_count  (integer)
//   lost_ip_count      (integer)
//   recovery_count     (integer, sum of all three recovery counters)
//   restart_sta_count  (integer, times bb_wifi_restart_sta was invoked)
//   disconnect_rssi    (integer, RSSI at most recent disconnect)
//   reason_histogram   (object, compact view: lost_ip/egress_dead/no_ip_watchdog
//                        sentinels + top non-zero standard reason)
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
    bb_json_obj_set_int   (obj, "restart_sta_count",  (int64_t)bb_wifi_get_restart_sta_count());
    bb_json_obj_set_int   (obj, "disconnect_rssi",    (int64_t)bb_wifi_get_disconnect_rssi());

    // Compact reason histogram: the three sentinel buckets + single top standard reason.
    uint16_t hist[256];
    bb_wifi_get_reason_histogram(hist, 256);

    uint16_t top_count = 0;
    uint16_t top_code  = wifi_hist_top_reason(hist, &top_count);

    bb_json_t h = bb_json_obj_new();
    bb_json_obj_set_int(h, "lost_ip",          (int64_t)hist[99]);
    bb_json_obj_set_int(h, "egress_dead",       (int64_t)hist[100]);
    bb_json_obj_set_int(h, "no_ip_watchdog",    (int64_t)hist[101]);
    bb_json_obj_set_int(h, "top_reason_code",   (int64_t)top_code);
    bb_json_obj_set_int(h, "top_reason_count",  (int64_t)top_count);
    bb_json_obj_set_obj(obj, "reason_histogram", h);
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
