// bb_net_health ESP-IDF glue — /api/health "net" section + retained SSE topic.
//
// Call order:
//   bb_net_health_register_health() — before bb_http_server_start
//   bb_net_health_attach_sse()      — in regular-tier init (after bb_event_routes)
//
// The 5-second evaluator reads bb_wifi_get_info() and bb_mqtt_get_stats(), runs
// bb_net_health_eval against a static module state, and publishes to the
// "net.health" retained SSE topic ONLY when state or early_warning changes.
//
// Note: bb_net_health_attach_sse must be called before the server starts
// serving SSE clients so the initial snapshot populates the retained ring.
#include "bb_net_health.h"
#include "bb_health.h"
#include "bb_wifi.h"
#include "bb_mqtt.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_timer.h"
#if CONFIG_BB_PUB_ADAPTIVE_BACKOFF
#include "bb_pub.h"
#endif

#include <string.h>
#include <stdio.h>

static const char *TAG = "bb_net_health";

#define BB_NET_HEALTH_TOPIC "net.health"
#define BB_NET_HEALTH_EVAL_PERIOD_US (5ULL * 1000000ULL)  // 5 seconds

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static bb_net_health_state_t s_state;       // hysteresis state (zero-init = valid)
static bb_event_topic_t      s_topic = NULL;
static bb_net_state_t        s_last_published_state    = (bb_net_state_t)-1;
static bool                  s_last_published_warn     = false;
static bool                  s_last_published_throttled = false;
static bb_periodic_timer_t   s_timer = NULL;
#if CONFIG_BB_PUB_ADAPTIVE_BACKOFF
static uint32_t              s_baseline_interval_ms = 0;  // captured before first throttle
#endif

// Cached health snapshot — written only by eval_cb (and the initial snapshot in
// attach_sse).  net_section_get reads this cache instead of calling
// bb_net_health_eval, preventing /api/health polls from injecting extra samples
// into the hysteresis state.
typedef struct {
    int8_t   rssi;
    bool     mqtt_connected;
    uint32_t mqtt_reconnect_count;
    uint32_t last_disconnect_reason;
    uint32_t disc_age_s;
    bb_net_state_t state;
    bool     early_warning;
    bool     throttled;
} bb_net_health_cache_t;

static bb_net_health_cache_t s_cache;  // zero-init; valid once attach_sse writes it

// JSON-Schema for the "net" health section.
static const char k_net_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"mqtt_connected\":{\"type\":\"boolean\"},"
    "\"mqtt_reconnect_count\":{\"type\":\"integer\"},"
    "\"last_disconnect_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"state\":{\"type\":\"string\"},"
    "\"early_warning\":{\"type\":\"boolean\"},"
    "\"throttled\":{\"type\":\"boolean\"}}}";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a bb_net_health_input_t from current system state.
static void build_input(bb_net_health_input_t *in)
{
    memset(in, 0, sizeof(*in));

    bb_wifi_info_t wi;
    if (bb_wifi_get_info(&wi) == BB_OK) {
        in->rssi     = wi.rssi;
        in->disc_age_s = wi.disc_age_s;
    }

    bb_mqtt_t mqtt = bb_mqtt_default();
    if (mqtt) {
        bb_mqtt_stats_t stats;
        if (bb_mqtt_get_stats(mqtt, &stats) == BB_OK) {
            in->mqtt_connected      = stats.connected;
            in->mqtt_reconnect_count = stats.reconnect_count;
        }
    }
}

// Publish a net.health payload to the SSE topic and update the module cache.
// payload fields: rssi, mqtt_connected, mqtt_reconnect_count,
// last_disconnect_reason, disc_age_s, state, early_warning.
static void publish_snapshot(const bb_net_health_output_t *out,
                             const bb_net_health_input_t  *in,
                             const bb_wifi_info_t         *wi,
                             bool throttled)
{
    // Update cache — net_section_get reads this; it must not call eval itself.
    s_cache.rssi                    = in->rssi;
    s_cache.mqtt_connected          = in->mqtt_connected;
    s_cache.mqtt_reconnect_count    = in->mqtt_reconnect_count;
    s_cache.last_disconnect_reason  = wi->disc_reason;
    s_cache.disc_age_s              = in->disc_age_s;
    s_cache.state                   = out->state;
    s_cache.early_warning           = out->early_warning;
    s_cache.throttled               = throttled;

    if (!s_topic) return;

    bb_json_t root = bb_json_obj_new();
    if (!root) return;

    bb_json_obj_set_number(root, "rssi",                   (double)in->rssi);
    bb_json_obj_set_bool(root,   "mqtt_connected",         in->mqtt_connected);
    bb_json_obj_set_number(root, "mqtt_reconnect_count",   (double)in->mqtt_reconnect_count);
    bb_json_obj_set_number(root, "last_disconnect_reason", (double)wi->disc_reason);
    bb_json_obj_set_number(root, "disc_age_s",             (double)in->disc_age_s);
    bb_json_obj_set_string(root, "state",                  bb_net_state_str(out->state));
    bb_json_obj_set_bool(root,   "early_warning",          out->early_warning);
    bb_json_obj_set_bool(root,   "throttled",              throttled);

    char *json = bb_json_serialize(root);
    bb_json_free(root);

    if (json) {
        bb_event_post(s_topic, (int32_t)out->state, json, strlen(json));
        bb_json_free_str(json);
    }
}

// ---------------------------------------------------------------------------
// Evaluator timer callback (fires every 5 s on ESP-IDF)
// ---------------------------------------------------------------------------

static void eval_cb(void *arg)
{
    (void)arg;

    bb_net_health_input_t in;
    build_input(&in);

    bb_wifi_info_t wi;
    memset(&wi, 0, sizeof(wi));
    bb_wifi_get_info(&wi);

    bb_net_health_output_t out;
    bb_net_health_eval(&s_state, &in, &out);

#if CONFIG_BB_PUB_ADAPTIVE_BACKOFF
    bool was_throttled = s_state.throttled;
    bool now_throttled = bb_net_health_throttle_decision(
        &s_state, CONFIG_BB_PUB_ADAPTIVE_SAMPLES);

    if (now_throttled && !was_throttled) {
        // Capture baseline (configured/persisted value) before first throttle.
        s_baseline_interval_ms = bb_pub_get_interval_ms();
        bb_pub_set_interval_volatile_ms((uint32_t)CONFIG_BB_PUB_ADAPTIVE_SLOW_MS);
        bb_log_i(TAG, "throttle: slowing bb_pub to %u ms (was %u ms)",
                 (unsigned)CONFIG_BB_PUB_ADAPTIVE_SLOW_MS,
                 (unsigned)s_baseline_interval_ms);
    } else if (!now_throttled && was_throttled) {
        // Recovery: restore captured baseline via volatile path (no NVS write).
        if (s_baseline_interval_ms > 0) {
            bb_pub_set_interval_volatile_ms(s_baseline_interval_ms);
            bb_log_i(TAG, "throttle: restoring bb_pub to %u ms",
                     (unsigned)s_baseline_interval_ms);
        }
        s_baseline_interval_ms = 0;
    }
    bool currently_throttled = now_throttled;
#else
    bool currently_throttled = false;
#endif

    // Publish only when state, early_warning, OR throttled flag differs from
    // the last published values.  This ensures:
    //  - A sustained throttle publishes ONCE on entry and ONCE on exit.
    //  - Every real state/warning transition is still captured.
    if (out.state != s_last_published_state ||
        out.early_warning != s_last_published_warn ||
        currently_throttled != s_last_published_throttled) {
        publish_snapshot(&out, &in, &wi, currently_throttled);
        s_last_published_state     = out.state;
        s_last_published_warn      = out.early_warning;
        s_last_published_throttled = currently_throttled;
    }
}

// ---------------------------------------------------------------------------
// /api/health "net" section callback
// ---------------------------------------------------------------------------

static void net_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;

    // Read from the cache written by eval_cb (and by the initial snapshot in
    // attach_sse).  Do NOT call bb_net_health_eval here — HTTP polls must not
    // inject samples into the hysteresis state.  Staleness is at most 5 s.
    bb_json_obj_set_number(section, "rssi",                   (double)s_cache.rssi);
    bb_json_obj_set_bool(section,   "mqtt_connected",         s_cache.mqtt_connected);
    bb_json_obj_set_number(section, "mqtt_reconnect_count",   (double)s_cache.mqtt_reconnect_count);
    bb_json_obj_set_number(section, "last_disconnect_reason", (double)s_cache.last_disconnect_reason);
    bb_json_obj_set_number(section, "disc_age_s",             (double)s_cache.disc_age_s);
    bb_json_obj_set_string(section, "state",                  bb_net_state_str(s_cache.state));
    bb_json_obj_set_bool(section,   "early_warning",          s_cache.early_warning);
    bb_json_obj_set_bool(section,   "throttled",              s_cache.throttled);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void bb_net_health_register_health(void)
{
    bb_health_register_section("net", net_section_get, NULL, k_net_schema);
}

bb_err_t bb_net_health_attach_sse(void)
{
    // Register the event topic so bb_event_post can target it.
    bb_err_t err = bb_event_topic_register(BB_NET_HEALTH_TOPIC, &s_topic);
    if (err != BB_OK) {
        bb_log_w(TAG, "topic register failed: %d", (int)err);
        return err;
    }

    // Attach as a retained SSE topic.
    err = bb_event_routes_attach_ex(BB_NET_HEALTH_TOPIC, /*retained=*/true);
    if (err != BB_OK) {
        bb_log_w(TAG, "attach_ex failed: %d", (int)err);
        return err;
    }

    // Publish initial snapshot so the retained ring is non-empty from T=0.
    {
        bb_net_health_input_t in;
        build_input(&in);

        bb_wifi_info_t wi;
        memset(&wi, 0, sizeof(wi));
        bb_wifi_get_info(&wi);

        bb_net_health_output_t out;
        bb_net_health_eval(&s_state, &in, &out);

        publish_snapshot(&out, &in, &wi, false);
        s_last_published_state     = out.state;
        s_last_published_warn      = out.early_warning;
        s_last_published_throttled = false;
    }

    // Start the 5-second periodic evaluator.
    err = bb_timer_periodic_create(eval_cb, NULL, "bb_net_health", &s_timer);
    if (err != BB_OK) {
        bb_log_w(TAG, "timer create failed: %d", (int)err);
        return err;
    }
    err = bb_timer_periodic_start(s_timer, BB_NET_HEALTH_EVAL_PERIOD_US);
    if (err != BB_OK) {
        bb_log_w(TAG, "timer start failed: %d", (int)err);
        return err;
    }

    bb_log_i(TAG, "SSE attached (retained), evaluator started");
    return BB_OK;
}
