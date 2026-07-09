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

// Find the top standard (non-breadboard-injected) reason in a
// BB_WIFI_DISC_COUNT-entry disconnect histogram. Pure; single implementation
// shared by /api/diag/net (platform/espidf/bb_net_health/bb_net_health_routes.c)
// and host tests (B1-486 finding #1/#2 — previously a static-inline copy
// reached into bb_wifi's private wifi_hist_priv.h from bb_net_health).
bb_wifi_disc_reason_t bb_wifi_reason_histogram_top(const uint16_t *hist, uint16_t *out_count)
{
    uint16_t top_count = 0;
    bb_wifi_disc_reason_t top_code = BB_WIFI_DISC_UNKNOWN;
    if (hist) {
        for (int i = 0; i < BB_WIFI_DISC_COUNT; i++) {
            if (i == BB_WIFI_DISC_BB_LOST_IP ||
                i == BB_WIFI_DISC_BB_EGRESS_DEAD ||
                i == BB_WIFI_DISC_BB_NO_IP_WATCHDOG) {
                continue;
            }
            if (hist[i] > top_count) {
                top_count = hist[i];
                top_code  = (bb_wifi_disc_reason_t)i;
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

// Human-readable, wire-stable label for a bb_wifi_disc_reason_t value (pure,
// host + device). See bb_wifi.h for the full contract: this label -- never
// a raw backend-specific numeric code -- is what crosses the wire. Fully
// reentrant/thread-safe — every branch returns a static string literal (no
// shared mutable buffer). An out-of-range value defensively falls back to
// "unknown" rather than formatting the ordinal into a string.
const char *bb_wifi_disc_reason_str(bb_wifi_disc_reason_t reason)
{
    switch (reason) {
    case BB_WIFI_DISC_UNKNOWN:           return "unknown";
    case BB_WIFI_DISC_AUTH_FAIL:         return "auth_fail";
    case BB_WIFI_DISC_ASSOC_FAIL:        return "assoc_fail";
    case BB_WIFI_DISC_HANDSHAKE_TIMEOUT: return "handshake_timeout";
    case BB_WIFI_DISC_CONNECTION_LOST:   return "connection_lost";
    case BB_WIFI_DISC_NO_AP_FOUND:       return "no_ap_found";
    case BB_WIFI_DISC_INACTIVITY:        return "inactivity";
    case BB_WIFI_DISC_DEAUTH:            return "deauth";
    case BB_WIFI_DISC_BEACON_TIMEOUT:    return "beacon_timeout";
    case BB_WIFI_DISC_BB_LOST_IP:        return "bb_lost_ip";
    case BB_WIFI_DISC_BB_EGRESS_DEAD:    return "bb_egress_dead";
    case BB_WIFI_DISC_BB_NO_IP_WATCHDOG: return "bb_no_ip_watchdog";
    default:                             return "unknown";
    }
}

// Map an esp_wifi WIFI_EVENT_STA_DISCONNECTED reason code onto the portable
// bb_wifi_disc_reason_t bucket (pure, host + device). Deliberately does NOT
// #include esp_wifi_types.h (this file compiles on host too) — the numeric
// literals below are the standard esp_wifi WIFI_REASON_* values, called out
// by name in each comment (mirrors bb_wifi_disc_reason_str's convention).
bb_wifi_disc_reason_t bb_wifi_map_esp_reason(uint16_t esp_code)
{
    switch (esp_code) {
    case 2:   return BB_WIFI_DISC_AUTH_FAIL;         // WIFI_REASON_AUTH_EXPIRE
    case 3:   return BB_WIFI_DISC_DEAUTH;            // WIFI_REASON_AUTH_LEAVE
    case 4:   return BB_WIFI_DISC_INACTIVITY;        // WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY
    case 15:  return BB_WIFI_DISC_HANDSHAKE_TIMEOUT; // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
    case 200: return BB_WIFI_DISC_BEACON_TIMEOUT;    // WIFI_REASON_BEACON_TIMEOUT
    case 201: return BB_WIFI_DISC_NO_AP_FOUND;       // WIFI_REASON_NO_AP_FOUND
    case 203: return BB_WIFI_DISC_ASSOC_FAIL;        // WIFI_REASON_ASSOC_FAIL
    case 204: return BB_WIFI_DISC_HANDSHAKE_TIMEOUT; // WIFI_REASON_HANDSHAKE_TIMEOUT
    case 205: return BB_WIFI_DISC_CONNECTION_LOST;   // WIFI_REASON_CONNECTION_FAIL
    default:  return BB_WIFI_DISC_UNKNOWN;
    }
}

// Map an Arduino WiFiS3 wl_status_t value onto the portable
// bb_wifi_disc_reason_t bucket (pure, host + device). Takes a plain int (not
// wl_status_t) so this file stays free of the Arduino WiFiS3.h include —
// the numeric literals below are the standard wl_status_t values.
bb_wifi_disc_reason_t bb_wifi_map_wl_status(int wl_status)
{
    switch (wl_status) {
    case 5: return BB_WIFI_DISC_CONNECTION_LOST; // WL_CONNECTION_LOST
    case 4: return BB_WIFI_DISC_ASSOC_FAIL;      // WL_CONNECT_FAILED
    case 1: return BB_WIFI_DISC_NO_AP_FOUND;     // WL_NO_SSID_AVAIL
    default: return BB_WIFI_DISC_UNKNOWN;
    }
}

// Pure cb-set/cb-NULL dispatch backing bb_wifi.c's private wifi_ota_validated()
// -- see bb_wifi.h for the full rationale (NULL cb == running image treated
// as trusted/permanent).
bool bb_wifi_ota_validated_eval(bb_wifi_ota_validated_fn cb)
{
    return cb ? cb() : true;
}
