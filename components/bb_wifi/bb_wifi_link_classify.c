// bb_wifi_link_classify — pure RSSI-bucket + net-mode classifiers.
//
// No ESP-IDF dependencies; compiles on host and device. Moved verbatim from
// bb_net_health (net_health teardown PR-B) — see bb_wifi.h for threshold and
// hysteresis constants.
#include "bb_wifi.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Classify raw RSSI into a state bucket (no hysteresis).
static bb_wifi_link_state_t rssi_bucket(int8_t rssi)
{
    // rssi >= 0 means no valid reading / not associated — treat as POOR.
    if (rssi >= 0) {
        return BB_WIFI_LINK_POOR;
    }
    if (rssi >= BB_WIFI_RSSI_GOOD) {
        return BB_WIFI_LINK_GOOD;
    }
    if (rssi >= BB_WIFI_RSSI_MARGINAL_LO) {
        return BB_WIFI_LINK_MARGINAL;
    }
    return BB_WIFI_LINK_POOR;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_wifi_link_state_t bb_wifi_classify_link(bb_wifi_link_hyst_t *st, int8_t rssi)
{
    bb_wifi_link_state_t raw = rssi_bucket(rssi);

    // Cold-start seeding: on the very first call, bypass hysteresis and report
    // the raw bucket directly so the boot snapshot reflects reality immediately.
    if (!st->initialized) {
        st->current_state = raw;
        st->initialized   = true;
    } else {
        // Hysteresis: downgrade requires BB_WIFI_HYST_DOWN consecutive
        // worse-bucket samples; upgrade requires BB_WIFI_HYST_UP consecutive
        // better-bucket samples.
        if (raw > st->current_state) {
            // Moving to a worse bucket.
            st->down_count++;
            st->up_count = 0;
            if (st->down_count >= BB_WIFI_HYST_DOWN) {
                st->current_state = raw;
                st->down_count    = 0;
            }
        } else if (raw < st->current_state) {
            // Moving to a better bucket.
            st->up_count++;
            st->down_count = 0;
            if (st->up_count >= BB_WIFI_HYST_UP) {
                st->current_state = raw;
                st->up_count      = 0;
            }
        } else {
            // Same bucket — clear transition counters.
            st->down_count = 0;
            st->up_count   = 0;
        }
    }

    return st->current_state;
}

const char *bb_wifi_link_state_str(bb_wifi_link_state_t state)
{
    switch (state) {
    case BB_WIFI_LINK_GOOD:     return "good";
    case BB_WIFI_LINK_MARGINAL: return "marginal";
    case BB_WIFI_LINK_POOR:     return "poor";
    default:                    return "poor";
    }
}

// ---------------------------------------------------------------------------
// WiFi discrimination mode
// ---------------------------------------------------------------------------

bb_wifi_mode_t bb_wifi_classify_mode(bool associated, bool has_ip)
{
    if (!associated) {
        return BB_WIFI_MODE_NOT_ASSOCIATED;
    }
    return has_ip ? BB_WIFI_MODE_OK : BB_WIFI_MODE_NO_IP;
}

const char *bb_wifi_mode_str(bb_wifi_mode_t mode)
{
    switch (mode) {
    case BB_WIFI_MODE_OK:             return "ok";
    case BB_WIFI_MODE_NO_IP:          return "no_ip";
    case BB_WIFI_MODE_NOT_ASSOCIATED: return "not_associated";
    default:                          return "not_associated";
    }
}
