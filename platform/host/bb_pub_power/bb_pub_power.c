// bb_pub_power — telemetry source satellite: power readings.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_power.h"
#include "bb_pub.h"
#include "bb_power.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>

#ifndef CONFIG_BB_PUB_POWER_AUTO_ATTACH
#define CONFIG_BB_PUB_POWER_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_power";

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "power" subtopic.
// ---------------------------------------------------------------------------

static bool power_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

    bb_power_handle_t h = bb_power_primary();
    if (!h) {
        // No primary power handle — skip this tick.
        return false;
    }

    bb_power_snapshot_t s;
    bb_power_snapshot(h, &s);

    bb_power_emit(obj, &s);

    return true;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_power_register(void)
{
    bb_err_t err = bb_pub_register_source("power", power_sample, NULL);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered power source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_source failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_power_init(void)
{
    return bb_pub_power_register();
}

#if CONFIG_BB_PUB_POWER_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_power, bb_pub_power_init);
#endif
