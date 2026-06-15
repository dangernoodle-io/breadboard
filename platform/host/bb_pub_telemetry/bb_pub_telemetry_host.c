// bb_pub_telemetry host twin — section get + patch logic + test hooks.
// Compiled on both host (test) and ESP-IDF (shared logic).
#include "bb_pub_telemetry.h"
#include "bb_pub.h"
#include "bb_clock.h"
#include "bb_json.h"
#include "bb_telemetry.h"
#include "bb_registry.h"
#include "bb_log.h"

#include <stdbool.h>
#include <stdint.h>

// Kconfig defaults for host builds.
#ifndef CONFIG_BB_PUB_TOPIC_PREFIX
#define CONFIG_BB_PUB_TOPIC_PREFIX "metrics"
#endif

static const char *TAG = "bb_pub_telemetry";

// Interval bounds (must match bb_pub.c / Kconfig range).
#define BB_PUB_INTERVAL_MS_MIN   1000UL
#define BB_PUB_INTERVAL_MS_MAX   3600000UL

// ---------------------------------------------------------------------------
// Section get
// ---------------------------------------------------------------------------

static void pub_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;

    bb_pub_status_t st;
    bb_pub_get_status(&st);

    uint32_t now_ms = bb_clock_now_ms();
    int32_t  age_ms = -1;
    if (st.published_ever) {
        age_ms = (int32_t)(now_ms - st.last_publish_ms);
    }

    bb_json_obj_set_number(section, "interval_ms",         (double)bb_pub_get_interval_ms());
    bb_json_obj_set_bool  (section, "enabled",             bb_pub_is_enabled());
    bb_json_obj_set_string(section, "topic_prefix",        CONFIG_BB_PUB_TOPIC_PREFIX);
    bb_json_obj_set_number(section, "source_count",        (double)st.source_count);
    bb_json_obj_set_number(section, "sink_count",          (double)st.sink_count);
    bb_json_obj_set_bool  (section, "last_publish_ok",     st.last_publish_ok);
    bb_json_obj_set_number(section, "last_publish_age_ms", (double)age_ms);
    bb_json_obj_set_bool  (section, "published_ever",      st.published_ever);

    bb_pub_buffer_stats_t buf;
    bb_pub_buffer_stats(&buf);
    bb_json_obj_set_number(section, "buffer_count",   (double)buf.count);
    bb_json_obj_set_number(section, "buffer_dropped", (double)buf.dropped);
}

// ---------------------------------------------------------------------------
// Section patch
// ---------------------------------------------------------------------------

static bb_err_t pub_section_patch(bb_json_t section_patch, void *ctx)
{
    (void)ctx;

    // interval_ms — optional field; validate and apply if present.
    double interval_val = 0.0;
    if (bb_json_obj_get_number(section_patch, "interval_ms", &interval_val)) {
        if (interval_val < (double)BB_PUB_INTERVAL_MS_MIN ||
            interval_val > (double)BB_PUB_INTERVAL_MS_MAX) {
            bb_log_w(TAG, "patch: interval_ms %.0f out of range [%lu, %lu]",
                     interval_val,
                     (unsigned long)BB_PUB_INTERVAL_MS_MIN,
                     (unsigned long)BB_PUB_INTERVAL_MS_MAX);
            return BB_ERR_INVALID_ARG;
        }
        bb_err_t err = bb_pub_set_interval_ms((uint32_t)interval_val);
        if (err != BB_OK) return err;
    }

    // enabled — optional field; apply if present.
    bool enabled_val = false;
    if (bb_json_obj_get_bool(section_patch, "enabled", &enabled_val)) {
        bb_err_t err = bb_pub_set_enabled(enabled_val);
        if (err != BB_OK) return err;
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_pub_telemetry_init(void)
{
    return bb_telemetry_register_section("publisher", pub_section_get,
                                         pub_section_patch, NULL);
}

#if CONFIG_BB_PUB_TELEMETRY_AUTOREGISTER
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_telemetry, bb_pub_telemetry_init);
#endif

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_PUB_TELEMETRY_TESTING

void bb_pub_telemetry_reset_for_test(void)
{
    bb_pub_test_reset();
}

void bb_pub_telemetry_section_get_for_test(bb_json_t section, void *ctx)
{
    pub_section_get(section, ctx);
}

bb_err_t bb_pub_telemetry_section_patch_for_test(bb_json_t patch, void *ctx)
{
    return pub_section_patch(patch, ctx);
}

#endif /* BB_PUB_TELEMETRY_TESTING */
