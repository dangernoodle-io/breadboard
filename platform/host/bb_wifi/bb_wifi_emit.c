// bb_wifi_emit — shared per-datum wifi section emitter.
// Compiled on both host (tests/bb_pub_wifi) and ESP-IDF (bb_wifi routes,
// bb_info, bb_health).  This is the single source of truth for the wifi
// section wire format.
//
// When disconnected all numeric fields are 0/false and string fields are
// empty/"0.0.0.0", matching the current /api/wifi behaviour.
//
// B1-486: the recovery counters (no_ip_recoveries, egress_dead_count,
// lost_ip_count, recovery_count) and reason_histogram have moved to
// GET /api/diag/net (bb_net_health) — that is now the single source of truth
// for recovery counters. /api/wifi keeps only connection-state fields plus
// restart_sta_count/disconnect_rssi (not recovery-count duplicates).
//
// roam_count/roam_age_s (B1-497) are likewise consolidated onto the
// /api/diag/net + net.health discriminator surface only — no longer
// duplicated here (net-health SSOT, wifi-netmode PR).
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
    bb_json_obj_set_int   (obj, "disc_reason",      (int64_t)info->disc_reason);
    bb_json_obj_set_int   (obj, "disc_age_s",       (int64_t)info->disc_age_s);
    bb_json_obj_set_int   (obj, "retry_count",      (int64_t)info->retry_count);
    bb_json_obj_set_int   (obj, "restart_sta_count",  (int64_t)bb_wifi_get_restart_sta_count());
    bb_json_obj_set_int   (obj, "disconnect_rssi",    (int64_t)bb_wifi_get_disconnect_rssi());
}

// Find the top standard (non-sentinel) reason in a 256-entry disconnect
// histogram. Pure; single implementation shared by /api/diag/net
// (platform/espidf/bb_net_health/bb_net_health_routes.c) and host tests
// (B1-486 finding #1/#2 — previously a static-inline copy reached into
// bb_wifi's private wifi_hist_priv.h from bb_net_health).
uint8_t bb_wifi_reason_histogram_top(const uint16_t *hist, uint16_t *out_count)
{
    uint16_t top_count = 0;
    uint8_t  top_code  = 0;
    if (hist) {
        for (int i = 0; i < 256; i++) {
            if (i == BB_WIFI_REASON_BB_LOST_IP ||
                i == BB_WIFI_REASON_BB_EGRESS_DEAD ||
                i == BB_WIFI_REASON_BB_NO_IP_WATCHDOG) {
                continue;
            }
            if (hist[i] > top_count) {
                top_count = hist[i];
                top_code  = (uint8_t)i;
            }
        }
    }
    if (out_count) *out_count = top_count;
    return top_code;
}

// Pure roam-detection predicate (B1-497, observe-only): true iff a prior
// BSSID was cached (non-zero) and the new BSSID differs from it. The first
// connect since boot (all-zero prior BSSID) is never a roam. Host-testable;
// the ESP-IDF STA_CONNECTED handler (bb_wifi.c) is the sole caller in
// production. NOT wired to any recovery action — detection only.
bool bb_wifi_is_roam(const uint8_t prior_bssid[6], const uint8_t new_bssid[6])
{
    static const uint8_t zero_bssid[6] = {0};
    if (!prior_bssid || !new_bssid) return false;
    if (memcmp(prior_bssid, zero_bssid, 6) == 0) return false; // first connect
    return memcmp(prior_bssid, new_bssid, 6) != 0;
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
