// bb_wifi_emit — pure wifi status helpers with no bb_json dependency.
// Compiled on both host (tests) and ESP-IDF.
//
// PR1 (KB 781): the bb_json_t-based JSON emitters (bb_wifi_emit_section,
// bb_wifi_emit_status) moved to bb_wifi_http (platform/host/bb_wifi_http/
// bb_wifi_http_emit.c) so the bb_wifi STA core sheds its bb_json dependency.
// This file keeps the pure, bb_json-free helpers only.
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

#include <string.h>

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

// Human-readable name for a WiFi disconnect reason code (pure, host +
// device). Covers the standard esp_wifi reason codes most commonly seen in
// the field plus the three breadboard sentinels. Deliberately does NOT
// #include esp_wifi_types.h (this file compiles on host too) — the numeric
// literals below are the standard esp_wifi WIFI_REASON_* values, called out
// by name in each comment. Fully reentrant/thread-safe — every branch
// returns a static string literal (no shared mutable buffer); the numeric
// reason code is already shown by every caller via `reason=%u`, so the
// unmapped-code branch returns the fixed literal "other" rather than
// formatting the code into a string.
const char *bb_wifi_disc_reason_str(uint8_t reason)
{
    switch (reason) {
    case 0:   return "unknown";
    case 2:   return "auth_expire";              // WIFI_REASON_AUTH_EXPIRE
    case 3:   return "auth_leave";                // WIFI_REASON_AUTH_LEAVE
    case 4:   return "disassoc_inactivity";       // WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY
    case 15:  return "4way_handshake_timeout";    // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
    case 200: return "beacon_timeout";            // WIFI_REASON_BEACON_TIMEOUT
    case 201: return "no_ap_found";               // WIFI_REASON_NO_AP_FOUND
    case 203: return "assoc_fail";                // WIFI_REASON_ASSOC_FAIL
    case 204: return "handshake_timeout";         // WIFI_REASON_HANDSHAKE_TIMEOUT
    case 205: return "connection_fail";           // WIFI_REASON_CONNECTION_FAIL
    case BB_WIFI_REASON_BB_LOST_IP:        return "bb_lost_ip";
    case BB_WIFI_REASON_BB_EGRESS_DEAD:    return "bb_egress_dead";
    case BB_WIFI_REASON_BB_NO_IP_WATCHDOG: return "bb_no_ip_watchdog";
    default:  return "other";
    }
}
