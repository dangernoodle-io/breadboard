// bb_pub_wifi — telemetry source satellite: full wifi connection info.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_wifi.h"
#include "bb_pub.h"
#include "bb_wifi.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <string.h>

#ifndef CONFIG_BB_PUB_WIFI_AUTO_ATTACH
#define CONFIG_BB_PUB_WIFI_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_wifi";

// ---------------------------------------------------------------------------
// Host test stub state (only when BB_PUB_WIFI_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_PUB_WIFI_TESTING
static bb_pub_wifi_test_info_t s_test_info;

void bb_pub_wifi_test_set_info(const bb_pub_wifi_test_info_t *info)
{
    if (!info) {
        memset(&s_test_info, 0, sizeof(s_test_info));
        return;
    }
    s_test_info = *info;
}

void bb_pub_wifi_test_set_rssi(bool connected, int8_t rssi)
{
    memset(&s_test_info, 0, sizeof(s_test_info));
    s_test_info.connected = connected;
    s_test_info.rssi      = rssi;
}
#endif /* BB_PUB_WIFI_TESTING */

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "wifi" subtopic.
// ---------------------------------------------------------------------------

static bool wifi_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

#ifdef BB_PUB_WIFI_TESTING
    if (!s_test_info.connected) return false;

    // Build a bb_wifi_info_t from the test stub so bb_wifi_emit_section
    // handles the bssid formatting and integer-rssi precision fix.
    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    info.connected   = s_test_info.connected;
    info.rssi        = s_test_info.rssi;
    strncpy(info.ssid, s_test_info.ssid, sizeof(info.ssid) - 1);
    memcpy(info.bssid, s_test_info.bssid, sizeof(info.bssid));
    strncpy(info.ip, s_test_info.ip, sizeof(info.ip) - 1);
    info.disc_reason = s_test_info.disc_reason;
    info.disc_age_s  = s_test_info.disc_age_s;
    info.retry_count = s_test_info.retry_count;
    bb_wifi_emit_section(obj, &info);
    return true;
#else
    if (!bb_wifi_has_ip()) return false;

    bb_wifi_info_t info;
    bb_err_t rc = bb_wifi_get_info(&info);
    if (rc != BB_OK) return false;

    bb_wifi_emit_section(obj, &info);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_wifi_register(void)
{
    bb_err_t err = bb_pub_register_source("wifi", wifi_sample, NULL);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered wifi source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_source failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_wifi_init(void)
{
    return bb_pub_wifi_register();
}

#if CONFIG_BB_PUB_WIFI_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_wifi, bb_pub_wifi_init);
#endif
