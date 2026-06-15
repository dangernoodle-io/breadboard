// bb_pub_health — telemetry source satellite: operational health fields.
// Compiled on both host (tests) and ESP-IDF.
//
// Emits the operationally-useful fields from /api/health that are not
// already covered by bb_pub_info (which handles reset_reason, ota_validated).
//
// Fields published:
//   ok             — bool  (true when wifi connected AND OTA validated)
//   mqtt_enabled   — bool  (bb_mqtt_default() != NULL)
//   mqtt_connected — bool  (bb_mqtt_is_connected(default handle))
//
// Always publishes (returns true) to provide a continuous health heartbeat.
#include "bb_pub_health.h"
#include "bb_pub.h"
#include "bb_mqtt.h"
#include "bb_ota_validator.h"
#include "bb_wifi.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>

#ifndef CONFIG_BB_PUB_HEALTH_AUTO_ATTACH
#define CONFIG_BB_PUB_HEALTH_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_health";

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "health" subtopic.
// ---------------------------------------------------------------------------

static bool health_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

    // ok: wifi connected and OTA slot validated
    bool wifi_connected = bb_wifi_has_ip();
    bool ota_ok         = bb_ota_is_validated();
    bool ok             = wifi_connected && ota_ok;
    bb_json_obj_set_bool(obj, "ok", ok);

    // mqtt: gracefully absent when no MQTT client was started
    bb_mqtt_t h         = bb_mqtt_default();
    bool mqtt_enabled   = (h != NULL);
    bool mqtt_connected = mqtt_enabled && bb_mqtt_is_connected(h);
    bb_json_obj_set_bool(obj, "mqtt_enabled",   mqtt_enabled);
    bb_json_obj_set_bool(obj, "mqtt_connected", mqtt_connected);

    return true;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_health_register(void)
{
    bb_err_t err = bb_pub_register_source("health", health_sample, NULL);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered health source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_source failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_health_init(void)
{
    return bb_pub_health_register();
}

#if CONFIG_BB_PUB_HEALTH_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_health, bb_pub_health_init);
#endif
