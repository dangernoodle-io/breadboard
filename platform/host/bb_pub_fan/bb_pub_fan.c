// bb_pub_fan — telemetry source satellite: fan readings.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_fan.h"
#include "bb_pub.h"
#include "bb_fan.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>

#ifndef CONFIG_BB_PUB_FAN_AUTO_ATTACH
#define CONFIG_BB_PUB_FAN_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_fan";

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "fan" subtopic.
// ---------------------------------------------------------------------------

static bool fan_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

    bb_fan_handle_t h = bb_fan_primary();
    if (!h) {
        // No primary fan — skip this tick.
        return false;
    }

    bb_fan_snapshot_t s;
    bb_fan_snapshot(h, &s);

#ifndef CONFIG_BB_FAN_AUTOFAN
    bb_fan_emit(obj, &s);
#else
    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);
    bb_fan_emit(obj, &s, &tel);
#endif

    return true;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_fan_register(void)
{
    bb_err_t err = bb_pub_register_source("fan", fan_sample, NULL);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered fan source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_source failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_fan_init(void)
{
    return bb_pub_fan_register();
}

#if CONFIG_BB_PUB_FAN_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_fan, bb_pub_fan_init);
#endif
