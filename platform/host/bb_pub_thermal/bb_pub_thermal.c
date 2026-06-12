// bb_pub_thermal — telemetry source satellite: aggregate temperature readings.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_thermal.h"
#include "bb_pub.h"
#include "bb_fan.h"
#include "bb_power.h"
#include "bb_temp.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <math.h>

#ifndef CONFIG_BB_PUB_THERMAL_AUTO_ATTACH
#define CONFIG_BB_PUB_THERMAL_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_thermal";

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "thermal" subtopic.
// ---------------------------------------------------------------------------

static bool thermal_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

    bool any_present = false;

    // SoC temperature (bb_temp)
    float soc_c = 0.0f;
    if (bb_temp_read_soc(&soc_c)) {
        bb_json_obj_set_number(obj, "soc_c", (double)soc_c);
        any_present = true;
    } else {
        bb_json_obj_set_null(obj, "soc_c");
    }

    // VR temperature (bb_power) — publish if primary exists; null when temp unavailable.
    bb_power_handle_t ph = bb_power_primary();
    if (ph) {
        bb_power_snapshot_t ps;
        bb_power_snapshot(ph, &ps);
        if (ps.temp_c >= 0) {
            bb_json_obj_set_number(obj, "vr_c", (double)ps.temp_c);
        } else {
            bb_json_obj_set_null(obj, "vr_c");
        }
        any_present = true;
    } else {
        bb_json_obj_set_null(obj, "vr_c");
    }

    // ASIC die temperature + board temperature (bb_fan) — publish if primary exists.
    bb_fan_handle_t fh = bb_fan_primary();
    if (fh) {
        bb_fan_snapshot_t fs;
        bb_fan_snapshot(fh, &fs);
        if (!isnan(fs.die_c)) {
            bb_json_obj_set_number(obj, "asic_c", (double)fs.die_c);
        } else {
            bb_json_obj_set_null(obj, "asic_c");
        }
        if (!isnan(fs.board_c)) {
            bb_json_obj_set_number(obj, "board_c", (double)fs.board_c);
        } else {
            bb_json_obj_set_null(obj, "board_c");
        }
        any_present = true;
    } else {
        bb_json_obj_set_null(obj, "asic_c");
        bb_json_obj_set_null(obj, "board_c");
    }

    // Skip when nothing is available.
    return any_present;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_thermal_register(void)
{
    bb_err_t err = bb_pub_register_source("thermal", thermal_sample, NULL);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered thermal source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_source failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_thermal_init(void)
{
    return bb_pub_thermal_register();
}

#if CONFIG_BB_PUB_THERMAL_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_thermal, bb_pub_thermal_init);
#endif
