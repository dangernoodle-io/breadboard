// bb_pub_thermal — telemetry source satellite: aggregate temperature readings.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_thermal.h"
#include "bb_pub.h"
#include "bb_thermal.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>

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

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    bool any_present = false;

    // SoC — always emit key; null when absent.
    if (v.soc_present) {
        bb_json_obj_set_number(obj, "soc_c", (double)v.soc_c);
        any_present = true;
    } else {
        bb_json_obj_set_null(obj, "soc_c");
    }

    // VR — omit key entirely when no hardware; null when hardware present but no reading.
    if (v.vr_hw_present) {
        if (v.vr_present) {
            bb_json_obj_set_number(obj, "vr_c", (double)v.vr_c);
        } else {
            bb_json_obj_set_null(obj, "vr_c");
        }
        any_present = true;
    }
    // else: no power hardware — omit vr_c entirely.

    // ASIC + board — omit both keys when no fan hardware; null per-channel when hardware
    // present but no reading.
    if (v.fan_hw_present) {
        if (v.asic_present) {
            bb_json_obj_set_number(obj, "asic_c", (double)v.asic_c);
        } else {
            bb_json_obj_set_null(obj, "asic_c");
        }
        if (v.board_present) {
            bb_json_obj_set_number(obj, "board_c", (double)v.board_c);
        } else {
            bb_json_obj_set_null(obj, "board_c");
        }
        any_present = true;
    }
    // else: no fan hardware — omit asic_c and board_c entirely.

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
