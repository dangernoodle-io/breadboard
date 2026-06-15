// bb_pub_wifi — telemetry source satellite: full wifi connection info.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_wifi.h"
#include "bb_pub.h"
#include "bb_wifi.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <stdio.h>
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
// Helper: format bssid bytes as "aa:bb:cc:dd:ee:ff"
// ---------------------------------------------------------------------------

static void format_bssid(const uint8_t *bssid, char *out, size_t out_size)
{
    snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2],
             bssid[3], bssid[4], bssid[5]);
}

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "wifi" subtopic.
// ---------------------------------------------------------------------------

static bool wifi_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

#ifdef BB_PUB_WIFI_TESTING
    if (!s_test_info.connected) return false;

    char bssid[18];
    format_bssid(s_test_info.bssid, bssid, sizeof(bssid));

    bb_json_obj_set_string(obj, "ssid",        s_test_info.ssid);
    bb_json_obj_set_string(obj, "bssid",       bssid);
    bb_json_obj_set_number(obj, "rssi",        (double)s_test_info.rssi);
    bb_json_obj_set_string(obj, "ip",          s_test_info.ip);
    bb_json_obj_set_bool  (obj, "connected",   s_test_info.connected);
    bb_json_obj_set_number(obj, "disc_reason", (double)s_test_info.disc_reason);
    bb_json_obj_set_number(obj, "disc_age_s",  (double)s_test_info.disc_age_s);
    bb_json_obj_set_number(obj, "retry_count", (double)s_test_info.retry_count);
    return true;
#else
    if (!bb_wifi_has_ip()) return false;

    bb_wifi_info_t info;
    bb_err_t rc = bb_wifi_get_info(&info);
    if (rc != BB_OK) return false;

    char bssid[18];
    format_bssid(info.bssid, bssid, sizeof(bssid));

    bb_json_obj_set_string(obj, "ssid",        info.ssid);
    bb_json_obj_set_string(obj, "bssid",       bssid);
    bb_json_obj_set_number(obj, "rssi",        (double)info.rssi);
    bb_json_obj_set_string(obj, "ip",          info.ip);
    bb_json_obj_set_bool  (obj, "connected",   info.connected);
    bb_json_obj_set_number(obj, "disc_reason", (double)info.disc_reason);
    bb_json_obj_set_number(obj, "disc_age_s",  (double)info.disc_age_s);
    bb_json_obj_set_number(obj, "retry_count", (double)info.retry_count);
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
