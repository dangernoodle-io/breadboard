// bb_pub_wifi — telemetry source satellite: wifi RSSI.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_wifi.h"
#include "bb_pub.h"
#include "bb_wifi.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>

#ifndef CONFIG_BB_PUB_WIFI_AUTO_ATTACH
#define CONFIG_BB_PUB_WIFI_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_wifi";

// ---------------------------------------------------------------------------
// Host test stub state (only when BB_PUB_WIFI_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_PUB_WIFI_TESTING
static bool  s_test_connected = false;
static int8_t s_test_rssi     = 0;

void bb_pub_wifi_test_set_rssi(bool connected, int8_t rssi)
{
    s_test_connected = connected;
    s_test_rssi      = rssi;
}
#endif /* BB_PUB_WIFI_TESTING */

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "wifi" subtopic.
// ---------------------------------------------------------------------------

static bool wifi_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

#ifdef BB_PUB_WIFI_TESTING
    if (!s_test_connected) return false;
    bb_json_obj_set_number(obj, "rssi", (double)s_test_rssi);
    return true;
#else
    if (!bb_wifi_has_ip()) return false;

    int8_t rssi = 0;
    bb_err_t rc = bb_wifi_get_rssi(&rssi);
    if (rc != BB_OK) return false;

    bb_json_obj_set_number(obj, "rssi", (double)rssi);
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
