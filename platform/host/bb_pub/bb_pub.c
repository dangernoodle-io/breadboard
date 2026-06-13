// bb_pub core — transport-agnostic telemetry publisher.
// Compiled on both host (tests) and ESP-IDF (linked alongside bb_pub_espidf.c).
//
// Timestamp note: bb_pub_tick_once injects a "ts" field using
// bb_clock_now_ms(), which returns uptime-milliseconds on all platforms
// (host: CLOCK_MONOTONIC, ESP-IDF: esp_timer_get_time()/1000). On devices
// with NTP synchronised wall-clock time, callers wanting epoch-ms should
// include the wall-clock timestamp themselves inside their sample_fn.
#include "bb_pub.h"
#include "bb_clock.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"

#include <string.h>
#include <stdio.h>

// CONFIG_BB_PUB_MAX_SOURCES, CONFIG_BB_PUB_MAX_SINKS, and
// CONFIG_BB_PUB_TOPIC_PREFIX are provided by Kconfig on ESP-IDF; supply
// defaults for host builds.
#ifndef CONFIG_BB_PUB_MAX_SOURCES
#define CONFIG_BB_PUB_MAX_SOURCES 8
#endif
#ifndef CONFIG_BB_PUB_MAX_SINKS
#define CONFIG_BB_PUB_MAX_SINKS 4
#endif
#ifndef CONFIG_BB_PUB_TOPIC_PREFIX
#define CONFIG_BB_PUB_TOPIC_PREFIX "metrics"
#endif

static const char *TAG = "bb_pub";

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

typedef struct {
    char            subtopic[64];
    bb_pub_sample_fn fn;
    void            *ctx;
} bb_pub_source_t;

static bb_pub_source_t s_sources[CONFIG_BB_PUB_MAX_SOURCES];
static int             s_source_count   = 0;
static bool            s_hwm_warned     = false;

static bb_pub_sink_t   s_sinks[CONFIG_BB_PUB_MAX_SINKS];
static int             s_sink_count     = 0;

// ---------------------------------------------------------------------------
// Public API — sinks
// ---------------------------------------------------------------------------

bb_err_t bb_pub_add_sink(const bb_pub_sink_t *sink)
{
    if (!sink || !sink->publish) return BB_ERR_INVALID_ARG;

    if (s_sink_count >= CONFIG_BB_PUB_MAX_SINKS) {
        return BB_ERR_NO_SPACE;
    }
    s_sinks[s_sink_count++] = *sink;
    return BB_OK;
}

void bb_pub_clear_sinks(void)
{
    s_sink_count = 0;
}

bb_err_t bb_pub_set_sink(const bb_pub_sink_t *sink)
{
    bb_pub_clear_sinks();
    if (!sink || !sink->publish) {
        return BB_OK;
    }
    return bb_pub_add_sink(sink);
}

// ---------------------------------------------------------------------------
// Public API — sources
// ---------------------------------------------------------------------------

bb_err_t bb_pub_register_source(const char *subtopic, bb_pub_sample_fn fn, void *ctx)
{
    if (!subtopic || !fn) return BB_ERR_INVALID_ARG;

    if (s_source_count >= CONFIG_BB_PUB_MAX_SOURCES) {
        return BB_ERR_NO_SPACE;
    }

    // High-watermark warning at cap-1.
    if (!s_hwm_warned && s_source_count == CONFIG_BB_PUB_MAX_SOURCES - 1) {
        bb_log_w(TAG, "source registry at high-watermark (%d/%d)",
                 s_source_count, CONFIG_BB_PUB_MAX_SOURCES);
        s_hwm_warned = true;
    }

    bb_pub_source_t *src = &s_sources[s_source_count++];
    strncpy(src->subtopic, subtopic, sizeof(src->subtopic) - 1);
    src->subtopic[sizeof(src->subtopic) - 1] = '\0';
    src->fn  = fn;
    src->ctx = ctx;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API — tick
// ---------------------------------------------------------------------------

bb_err_t bb_pub_tick_once(void)
{
    if (s_sink_count == 0) return BB_OK;

    // Take ONE timestamp for the entire cycle.
    uint32_t ts_ms = bb_clock_now_ms();

    const char *hostname = bb_nv_config_hostname();
    if (!hostname || hostname[0] == '\0') {
        hostname = "device";
    }

    char topic[192];

    for (int i = 0; i < s_source_count; i++) {
        bb_pub_source_t *src = &s_sources[i];

        bb_json_t obj = bb_json_obj_new();
        if (!obj) {
            bb_log_w(TAG, "tick: failed to allocate JSON obj for '%s'", src->subtopic);
            continue;
        }

        bool publish = src->fn(obj, src->ctx);
        if (!publish) {
            bb_json_free(obj);
            continue;
        }

        // Inject shared timestamp field (uptime-ms; see file-level note above).
        bb_json_obj_set_number(obj, "ts", (double)ts_ms);

        char *json = bb_json_serialize(obj);
        bb_json_free(obj);

        if (!json) {
            bb_log_w(TAG, "tick: failed to serialize '%s'", src->subtopic);
            continue;
        }

        snprintf(topic, sizeof(topic), "%s/%s/%s",
                 CONFIG_BB_PUB_TOPIC_PREFIX, hostname, src->subtopic);

        int json_len = (int)strlen(json);

        // Fan-out: deliver to every registered sink. A failing sink does not
        // abort delivery to the remaining sinks or to subsequent sources.
        for (int si = 0; si < s_sink_count; si++) {
            bb_err_t err = s_sinks[si].publish(s_sinks[si].ctx, topic, json, json_len);
            if (err != BB_OK) {
                bb_log_w(TAG, "sink[%d] publish failed for '%s': %d", si, src->subtopic, err);
            }
        }

        bb_json_free_str(json);
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Testing hooks
// ---------------------------------------------------------------------------

#ifdef BB_PUB_TESTING
void bb_pub_test_reset(void)
{
    s_source_count = 0;
    s_hwm_warned   = false;
    s_sink_count   = 0;
}
#endif
