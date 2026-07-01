// bb_pub_thermal — telemetry source satellite: aggregate temperature readings.
// Compiled on both host (tests) and ESP-IDF.
//
// Migration (telemetry-ssot): uses bb_pub_register_telemetry so the snapshot
// is gathered into bb_cache once per tick; SSE, sinks, and REST all read the
// SAME memoized serialization.  ts_ms is stamped at gather time.
#include "bb_pub_thermal.h"
#include "bb_pub.h"
#include "bb_thermal.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_clock.h"
#include "bb_openapi.h"
#include "bb_init.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef CONFIG_BB_PUB_THERMAL_AUTO_ATTACH
#define CONFIG_BB_PUB_THERMAL_AUTO_ATTACH 0
#endif

// Kconfig host fallback — matches the no-PSRAM Kconfig default.
// On ESP-IDF the build system supplies the real CONFIG_ value via sdkconfig.h.
#ifndef CONFIG_BB_PUB_TELEM_SNAP_MAX
#define CONFIG_BB_PUB_TELEM_SNAP_MAX 512
#endif

static const char *TAG = "bb_pub_thermal";

// ---------------------------------------------------------------------------
// Snapshot struct — ~44 bytes, well within the SNAP_MAX limit.
// ---------------------------------------------------------------------------

typedef struct {
    bb_thermal_values_t vals;  // bools + floats
    int64_t             ts_ms;
} bb_thermal_snap_t;

// Compile-time guard: thermal snap must fit in the scratch buffer (B1-434).
typedef char _thermal_snap_size_check[
    sizeof(bb_thermal_snap_t) <= CONFIG_BB_PUB_TELEM_SNAP_MAX ? 1 : -1];

// ---------------------------------------------------------------------------
// Gather — fills snap; skips tick when all sources absent.
// ---------------------------------------------------------------------------

static bool thermal_gather(void *snap_buf, void *ctx)
{
    (void)ctx;

    bb_thermal_snap_t *s = snap_buf;
    memset(s, 0, sizeof(*s));
    bb_thermal_collect(&s->vals);

    bool any_present = s->vals.soc_present || s->vals.vr_hw_present || s->vals.fan_hw_present;
    if (!any_present) return false;

    s->ts_ms = (int64_t)bb_clock_now_ms64();
    return true;
}

// ---------------------------------------------------------------------------
// Serialize — replicates the flat-field emit from the original thermal_sample.
// Uses same field names and null/omit semantics.
// ---------------------------------------------------------------------------

static void thermal_serialize(bb_json_t obj, const void *snap_raw)
{
    const bb_thermal_snap_t *s = snap_raw;
    const bb_thermal_values_t *v = &s->vals;

    // SoC — always emit key; null when absent.
    if (v->soc_present) {
        bb_json_obj_set_number(obj, "soc_c", (double)v->soc_c);
    } else {
        bb_json_obj_set_null(obj, "soc_c");
    }

    // VR — omit key entirely when no hardware; null when hardware present but no reading.
    if (v->vr_hw_present) {
        if (v->vr_present) {
            bb_json_obj_set_number(obj, "vr_c", (double)v->vr_c);
        } else {
            bb_json_obj_set_null(obj, "vr_c");
        }
    }

    // ASIC + board — omit both keys when no fan hardware.
    if (v->fan_hw_present) {
        if (v->asic_present) {
            bb_json_obj_set_number(obj, "asic_c", (double)v->asic_c);
        } else {
            bb_json_obj_set_null(obj, "asic_c");
        }
        if (v->board_present) {
            bb_json_obj_set_number(obj, "board_c", (double)v->board_c);
        } else {
            bb_json_obj_set_null(obj, "board_c");
        }
    }

    bb_json_obj_set_int(obj, "ts_ms", s->ts_ms);
}

// ---------------------------------------------------------------------------
// Schema + Registration
// ---------------------------------------------------------------------------

static const char k_thermal_telemetry_schema[] =
    "{\"title\":\"ThermalTelemetry\",\"x-sse-topic\":\"thermal\",\"type\":\"object\","
    "\"properties\":{"
    "\"soc_c\":{\"type\":[\"number\",\"null\"]},"
    "\"vr_c\":{\"type\":[\"number\",\"null\"]},"
    "\"asic_c\":{\"type\":[\"number\",\"null\"]},"
    "\"board_c\":{\"type\":[\"number\",\"null\"]},"
    "\"ts_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"soc_c\",\"ts_ms\"]}";

bb_err_t bb_pub_thermal_register(void)
{
    bb_pub_telemetry_cfg_t cfg = {
        .topic     = "thermal",
        .gather    = thermal_gather,
        .serialize = thermal_serialize,
        .snap_size = sizeof(bb_thermal_snap_t),
        .flags     = BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS,
        .ctx       = NULL,
    };

    bb_openapi_register_topic_schema("thermal", k_thermal_telemetry_schema, "ThermalTelemetry");

    bb_err_t err = bb_pub_register_telemetry(&cfg);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered thermal telemetry source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_telemetry failed: %d", err);
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
BB_INIT_REGISTER_PRE_HTTP(bb_pub_thermal, bb_pub_thermal_init);
#endif
