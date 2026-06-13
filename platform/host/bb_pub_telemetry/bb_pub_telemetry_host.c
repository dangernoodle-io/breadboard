// bb_pub_telemetry host twin — section get logic + test hooks.
// Compiled on both host (test) and ESP-IDF (shared logic).
#include "bb_pub_telemetry.h"
#include "bb_pub.h"
#include "bb_clock.h"
#include "bb_json.h"
#include "bb_telemetry.h"
#include "bb_registry.h"

#include <stdbool.h>
#include <stdint.h>

// Kconfig defaults for host builds.
#ifndef CONFIG_BB_PUB_INTERVAL_MS
#define CONFIG_BB_PUB_INTERVAL_MS 10000
#endif
#ifndef CONFIG_BB_PUB_TOPIC_PREFIX
#define CONFIG_BB_PUB_TOPIC_PREFIX "metrics"
#endif

// ---------------------------------------------------------------------------
// Section get (read-only)
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

    bb_json_obj_set_number(section, "interval_ms",         (double)CONFIG_BB_PUB_INTERVAL_MS);
    bb_json_obj_set_string(section, "topic_prefix",        CONFIG_BB_PUB_TOPIC_PREFIX);
    bb_json_obj_set_number(section, "source_count",        (double)st.source_count);
    bb_json_obj_set_number(section, "sink_count",          (double)st.sink_count);
    bb_json_obj_set_bool  (section, "last_publish_ok",     st.last_publish_ok);
    bb_json_obj_set_number(section, "last_publish_age_ms", (double)age_ms);
    bb_json_obj_set_bool  (section, "published_ever",      st.published_ever);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_pub_telemetry_init(void)
{
    // Read-only: pass NULL for patch_fn.
    return bb_telemetry_register_section("publisher", pub_section_get, NULL, NULL);
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

#endif /* BB_PUB_TELEMETRY_TESTING */
