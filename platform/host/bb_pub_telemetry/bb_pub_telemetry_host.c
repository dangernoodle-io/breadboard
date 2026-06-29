// bb_pub_telemetry host twin — section get + patch logic + test hooks.
// Compiled on both host (test) and ESP-IDF (shared logic).
#include "bb_pub_telemetry.h"
#include "bb_pub.h"
#include "bb_clock.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_telemetry.h"
#include "bb_registry.h"
#include "bb_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Kconfig defaults for host builds.
#ifndef CONFIG_BB_PUB_TOPIC_PREFIX
#define CONFIG_BB_PUB_TOPIC_PREFIX "metrics"
#endif
#ifndef CONFIG_BB_PUB_TELEM_SNAP_MAX
#define CONFIG_BB_PUB_TELEM_SNAP_MAX 256
#endif

static const char *TAG = "bb_pub_telemetry";

// Interval bounds (must match bb_pub.c / Kconfig range).
#define BB_PUB_INTERVAL_MS_MIN   1000UL
#define BB_PUB_INTERVAL_MS_MAX   3600000UL

// Maximum sinks captured in the meta snapshot.  Kept at 4 so meta_snap_t
// fits within CONFIG_BB_PUB_TELEM_SNAP_MAX=256 bytes even when MAX_SINKS=8.
#define BB_PUB_META_MAX_SINKS 4

// ---------------------------------------------------------------------------
// /meta telem snapshot
// ---------------------------------------------------------------------------

typedef struct {
    char  topic_prefix[48];
    int   sink_count;
    struct {
        char  transport[28];   /* "" = no transport label */
        bool  tls;
    } sinks[BB_PUB_META_MAX_SINKS];
} meta_snap_t;

// Compile-time guard: meta_snap_t must fit in the scratch buffer.
// A build failure here means the struct grew past the limit — reduce field sizes.
typedef char _meta_snap_size_check[
    sizeof(meta_snap_t) <= CONFIG_BB_PUB_TELEM_SNAP_MAX ? 1 : -1];

static bool meta_gather(void *snap_buf, void *ctx)
{
    (void)ctx;
    meta_snap_t *m = (meta_snap_t *)snap_buf;

    strncpy(m->topic_prefix, CONFIG_BB_PUB_TOPIC_PREFIX, sizeof(m->topic_prefix) - 1);
    m->topic_prefix[sizeof(m->topic_prefix) - 1] = '\0';

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    m->sink_count = st.sink_count;

    int cap = st.sink_count < BB_PUB_META_MAX_SINKS ? st.sink_count : BB_PUB_META_MAX_SINKS;
    for (int i = 0; i < cap; i++) {
        const char *transport = NULL;
        bool tls = false;
        bb_pub_sink_info(i, &transport, &tls);
        m->sinks[i].transport[0] = '\0';
        if (transport) {
            strncpy(m->sinks[i].transport, transport,
                    sizeof(m->sinks[i].transport) - 1);
            m->sinks[i].transport[sizeof(m->sinks[i].transport) - 1] = '\0';
        }
        m->sinks[i].tls = tls;
    }

    return true;   /* always publish provenance, even with zero sinks */
}

static void meta_serialize(bb_json_t obj, const void *snap)
{
    const meta_snap_t *m = (const meta_snap_t *)snap;

    bb_json_obj_set_string(obj, "topic_prefix", m->topic_prefix);
    bb_json_obj_set_int(obj, "sink_count", (int64_t)m->sink_count);

    bb_json_t arr = bb_json_arr_new();
    if (!arr) return;

    int cap = m->sink_count < BB_PUB_META_MAX_SINKS ? m->sink_count : BB_PUB_META_MAX_SINKS;
    for (int i = 0; i < cap; i++) {
        bb_json_t sink_obj = bb_json_obj_new();
        if (!sink_obj) continue;
        if (m->sinks[i].transport[0] != '\0') {
            bb_json_obj_set_string(sink_obj, "transport", m->sinks[i].transport);
            bb_json_obj_set_bool  (sink_obj, "tls",       m->sinks[i].tls);
        } else {
            bb_json_obj_set_null(sink_obj, "transport");
        }
        bb_json_arr_append_obj(arr, sink_obj);
        /* sink_obj ownership transferred to arr — do NOT free here */
    }

    bb_json_obj_set_arr(obj, "sinks", arr);
    /* arr ownership transferred to obj — do NOT free here */
}

// ---------------------------------------------------------------------------
// /api/telemetry publisher section — GET
// ---------------------------------------------------------------------------

static void pub_sinks_add(bb_json_t section)
{
    bb_pub_status_t st;
    bb_pub_get_status(&st);

    bb_json_t arr = bb_json_arr_new();
    if (!arr) return;

    int cap = st.sink_count < BB_PUB_META_MAX_SINKS ? st.sink_count : BB_PUB_META_MAX_SINKS;
    for (int i = 0; i < cap; i++) {
        const char *transport = NULL;
        bool tls = false;
        bb_pub_sink_info(i, &transport, &tls);
        bb_json_t sink_obj = bb_json_obj_new();
        if (!sink_obj) continue;
        if (transport) {
            bb_json_obj_set_string(sink_obj, "transport", transport);
            bb_json_obj_set_bool  (sink_obj, "tls",       tls);
        } else {
            bb_json_obj_set_null(sink_obj, "transport");
        }
        bb_json_arr_append_obj(arr, sink_obj);
        /* sink_obj ownership transferred to arr — do NOT free here */
    }

    bb_json_obj_set_arr(section, "sinks", arr);
    /* arr ownership transferred to section — do NOT free here */
}

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
    // available: true when the periodic worker was successfully started.
    // False on AUTOREGISTER=n builds or when bb_pub_start failed — a reboot
    // will NOT enable the publisher on such builds (B1-398).
    bb_json_obj_set_bool  (section, "available",           st.available);
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

    // Provenance: sinks[] array (B1-388 — transport/tls removed from payloads).
    pub_sinks_add(section);
}

// ---------------------------------------------------------------------------
// /api/telemetry publisher section — PATCH
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
        // Note (B1-398): if enabled_val=true but the publisher is not available
        // (AUTOREGISTER=n build), we still persist the value (harmless; takes
        // effect on a future build with AUTOREGISTER=y). The route handler
        // checks bb_pub_get_status().available after dispatch and returns
        // reboot_required=false + publisher_unavailable=true in that case.
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// /api/info "pub_sinks" section
// ---------------------------------------------------------------------------

static void info_pub_sinks_get(bb_json_t section, void *ctx)
{
    (void)ctx;

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    bb_json_obj_set_int(section, "sink_count", (int64_t)st.sink_count);
    bb_json_obj_set_string(section, "topic_prefix", CONFIG_BB_PUB_TOPIC_PREFIX);

    // sinks[] — provenance: which transports are active and whether TLS is on.
    pub_sinks_add(section);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_pub_telemetry_init(void)
{
    // Register /api/telemetry "publisher" section.
    static const char k_pub_schema_props[] =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"interval_ms\":{\"type\":\"number\"},"
        "\"enabled\":{\"type\":\"boolean\"},"
        "\"available\":{\"type\":\"boolean\","
            "\"description\":\"true when the publisher worker is running; "
            "false on AUTOREGISTER=n builds where a reboot will not start it\"},"
        "\"topic_prefix\":{\"type\":\"string\"},"
        "\"source_count\":{\"type\":\"number\"},"
        "\"sink_count\":{\"type\":\"number\"},"
        "\"last_publish_ok\":{\"type\":\"boolean\"},"
        "\"last_publish_age_ms\":{\"type\":\"number\"},"
        "\"published_ever\":{\"type\":\"boolean\"},"
        "\"buffer_count\":{\"type\":\"number\"},"
        "\"buffer_dropped\":{\"type\":\"number\"},"
        "\"sinks\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}}}";
    bb_err_t err = bb_telemetry_register_section_ex("publisher", pub_section_get,
                                                     pub_section_patch, NULL,
                                                     k_pub_schema_props);
    if (err != BB_OK) return err;

    // Register /api/info "pub_sinks" section: provenance for who receives telemetry.
    static const char k_info_schema[] =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"sink_count\":{\"type\":\"integer\"},"
        "\"topic_prefix\":{\"type\":\"string\"},"
        "\"sinks\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}}}";
    (void)bb_info_register_section("pub_sinks", info_pub_sinks_get, NULL, k_info_schema);

    // Register "meta" MQTT topic: serializes once per tick via bb_pub_register_telemetry.
    // BB_PUB_TELEM_SINKS only (no SSE — meta is a sink-delivery topic, not an event stream).
    static const bb_pub_telemetry_cfg_t k_meta_cfg = {
        .topic     = "meta",
        .gather    = meta_gather,
        .serialize = meta_serialize,
        .snap_size = sizeof(meta_snap_t),
        .flags     = BB_PUB_TELEM_SINKS,
        .ctx       = NULL,
    };
    err = bb_pub_register_telemetry(&k_meta_cfg);
    if (err != BB_OK) {
        // Not fatal — meta is provenance; log but continue.
        bb_log_w(TAG, "meta telem source registration failed: %d", err);
    }

    return BB_OK;
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
