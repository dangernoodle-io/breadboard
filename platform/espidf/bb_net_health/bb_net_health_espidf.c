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
#include "bb_cache.h"
#include "bb_health.h"
#include "bb_wifi.h"
#include "bb_mqtt.h"
#include "bb_tls.h"
#include "bb_sink_http.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_timer.h"
#include "bb_pub.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "bb_net_health";

#define BB_NET_HEALTH_TOPIC "net.health"
#define BB_NET_HEALTH_EVAL_PERIOD_US ((uint64_t)CONFIG_BB_NET_HEALTH_EVAL_PERIOD_S * 1000000ULL)

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static bb_net_health_state_t s_state;       // hysteresis state (zero-init = valid)
static bb_event_topic_t      s_topic = NULL;
static bb_net_state_t        s_last_published_state    = (bb_net_state_t)-1;
static bool                  s_last_published_warn     = false;
static bool                  s_last_published_throttled = false;
static bb_periodic_timer_t   s_timer = NULL;
// MQTT disconnect tracking: record the ms-timestamp when reconnect_count
// increases (the moment we detect a new disconnect).  0 means "no disconnect
// observed yet" (i.e. no reconnects have ever occurred).
static uint64_t              s_mqtt_disc_time_ms = 0;   // bb_timer_now_us()/1000 at last disc; _ms name is intentional (derived to mqtt_disc_age_s)
static uint32_t              s_last_reconnect_count = 0; // glue-side mirror for disc detection
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
    uint32_t mqtt_disc_age_s;
    uint32_t mqtt_disc_reason;
    uint32_t mqtt_tls_fail;
    bool     http_connected;
    int      http_consec_failures;
    int      http_tls_fail;
    int      http_last_status;
} bb_net_health_cache_t;

static bb_net_health_cache_t s_cache;       // zero-init; valid once attach_sse writes it
static SemaphoreHandle_t     s_cache_lock;  // protects s_cache against torn read/write

// JSON-Schema for the "net" health section.
static const char k_net_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"state\":{\"type\":\"string\"},"
    "\"early_warning\":{\"type\":\"boolean\"},"
    "\"throttled\":{\"type\":\"boolean\"},"
    "\"last_disconnect_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"mqtt\":{\"type\":\"object\",\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"reconnect_count\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"tls_fail\":{\"type\":\"integer\"}"
    "}},"
    "\"http\":{\"type\":\"object\",\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"consec_failures\":{\"type\":\"integer\"},"
    "\"tls_fail\":{\"type\":\"integer\"},"
    "\"last_status\":{\"type\":\"integer\"}"
    "}}}}";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a bb_net_health_input_t from current system state.
static void build_input(bb_net_health_input_t *in)
{
    memset(in, 0, sizeof(*in));

    bb_wifi_info_t wi;
    if (bb_wifi_get_info(&wi) == BB_OK) {
        in->rssi       = wi.rssi;
        in->disc_age_s = wi.disc_age_s;
    }

    bb_mqtt_t mqtt = bb_mqtt_default();
    if (mqtt) {
        bb_mqtt_stats_t stats;
        if (bb_mqtt_get_stats(mqtt, &stats) == BB_OK) {
            in->mqtt_connected       = stats.connected;
            in->mqtt_reconnect_count = stats.reconnect_count;

            // Detect a new MQTT disconnect by comparing reconnect_count.
            // When the count increases, record the current time as the
            // MQTT disconnect timestamp.  This is sampled every 5 s so the
            // age resolution is ~5 s, which is fine for the 60 s threshold.
            if (stats.reconnect_count > s_last_reconnect_count) {
                s_mqtt_disc_time_ms    = bb_timer_now_us() / 1000U;
                s_last_reconnect_count = stats.reconnect_count;
            }
        }
    }

    // Compute MQTT disconnect age.  If no disconnect has been observed
    // (s_mqtt_disc_time_ms == 0), leave mqtt_disc_age_s at 0 — the classifier
    // treats 0 as "no recent disconnect" because 0 < 60 only fires when
    // !mqtt_connected, and on a fresh boot the client will be connecting, not
    // yet disconnected.  Populate a non-zero age only when we've actually seen
    // a disconnect.
    if (s_mqtt_disc_time_ms > 0) {
        uint64_t now_ms    = bb_timer_now_us() / 1000U;
        uint64_t elapsed   = (now_ms >= s_mqtt_disc_time_ms)
                             ? (now_ms - s_mqtt_disc_time_ms)
                             : 0;
        in->mqtt_disc_age_s = (uint32_t)(elapsed / 1000U);
    }
}

// Publish a net.health payload to the SSE topic and update the module cache.
//
// The SSE payload carries only the 4 essential fields (rssi, state,
// early_warning, throttled) — worst case ~62 B — so it fits inside
// CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY even on consumers that set a small
// value like 128 (e.g. TaipanMiner).  The full 8-field detail is always
// available via GET /api/health "net" section (net_section_get below), which
// reads from the cache that is updated unconditionally here.
bb_err_t bb_net_health_get_status(bb_net_health_status_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (!s_cache_lock) return BB_ERR_INVALID_STATE;  // evaluator not up yet
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    out->state                  = s_cache.state;
    out->early_warning          = s_cache.early_warning;
    out->throttled              = s_cache.throttled;
    out->rssi                   = s_cache.rssi;
    out->mqtt_connected         = s_cache.mqtt_connected;
    out->mqtt_reconnect_count   = s_cache.mqtt_reconnect_count;
    out->last_disconnect_reason = s_cache.last_disconnect_reason;
    out->disc_age_s             = s_cache.disc_age_s;
    out->mqtt_disc_age_s        = s_cache.mqtt_disc_age_s;
    out->mqtt_disc_reason       = s_cache.mqtt_disc_reason;
    out->mqtt_tls_fail          = s_cache.mqtt_tls_fail;
    out->http_connected         = s_cache.http_connected;
    out->http_consec_failures   = s_cache.http_consec_failures;
    out->http_tls_fail          = s_cache.http_tls_fail;
    out->http_last_status       = s_cache.http_last_status;
    xSemaphoreGive(s_cache_lock);
    return BB_OK;
}

static void publish_snapshot(const bb_net_health_output_t *out,
                             const bb_net_health_input_t  *in,
                             const bb_wifi_info_t         *wi,
                             bool throttled,
                             bb_mqtt_disc_t mqtt_disc_reason,
                             bb_tls_fail_t  mqtt_tls_fail,
                             bb_sink_http_health_t http_h)
{
    bb_net_health_status_t snap = {
        .state                  = out->state,
        .early_warning          = out->early_warning,
        .throttled              = throttled,
        .rssi                   = in->rssi,
        .mqtt_connected         = in->mqtt_connected,
        .disc_age_s             = in->disc_age_s,
        .mqtt_reconnect_count   = in->mqtt_reconnect_count,
        .last_disconnect_reason = (uint32_t)wi->disc_reason,
        .mqtt_disc_age_s        = in->mqtt_disc_age_s,
        .mqtt_disc_reason       = (uint32_t)mqtt_disc_reason,
        .mqtt_tls_fail          = (uint32_t)mqtt_tls_fail,
        .http_connected         = http_h.connected,
        .http_consec_failures   = (uint32_t)http_h.consec_failures,
        .http_tls_fail          = (uint32_t)http_h.tls_fail,
        .http_last_status       = http_h.last_status,
    };

    // Update s_cache so bb_net_health_get_status (used by the pub source) can
    // read the latest snapshot without re-running eval.
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    s_cache.rssi                    = snap.rssi;
    s_cache.mqtt_connected          = snap.mqtt_connected;
    s_cache.mqtt_reconnect_count    = snap.mqtt_reconnect_count;
    s_cache.last_disconnect_reason  = snap.last_disconnect_reason;
    s_cache.disc_age_s              = snap.disc_age_s;
    s_cache.state                   = snap.state;
    s_cache.early_warning           = snap.early_warning;
    s_cache.throttled               = snap.throttled;
    s_cache.mqtt_disc_age_s         = snap.mqtt_disc_age_s;
    s_cache.mqtt_disc_reason        = snap.mqtt_disc_reason;
    s_cache.mqtt_tls_fail           = snap.mqtt_tls_fail;
    s_cache.http_connected          = snap.http_connected;
    s_cache.http_consec_failures    = snap.http_consec_failures;
    s_cache.http_tls_fail           = snap.http_tls_fail;
    s_cache.http_last_status        = snap.http_last_status;
    xSemaphoreGive(s_cache_lock);

    // Update bb_cache owned struct — SSE uses bb_cache_post, REST uses
    // bb_cache_serialize_into; both serialize from the same snapshot.
    bb_cache_update(BB_NET_HEALTH_TOPIC, &snap);
    bb_cache_post(BB_NET_HEALTH_TOPIC);
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

    // B1-362: capture disc_reason + tls_fail separately for publish_snapshot
    bb_mqtt_disc_t mqtt_disc_reason = BB_MQTT_DISC_NONE;
    bb_tls_fail_t  mqtt_tls_fail   = BB_TLS_FAIL_NONE;
    {
        bb_mqtt_t mqtt = bb_mqtt_default();
        if (mqtt) {
            bb_mqtt_stats_t stats;
            if (bb_mqtt_get_stats(mqtt, &stats) == BB_OK) {
                mqtt_disc_reason = stats.disc_reason;
                mqtt_tls_fail    = stats.tls_fail;
            }
        }
    }

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
    bb_sink_http_health_t http_h = {0};
    bb_sink_http_get_health(&http_h);

    if (out.state != s_last_published_state ||
        out.early_warning != s_last_published_warn ||
        currently_throttled != s_last_published_throttled) {
        publish_snapshot(&out, &in, &wi, currently_throttled, mqtt_disc_reason, mqtt_tls_fail, http_h);
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
    // Read from the bb_cache owned struct (updated by publish_snapshot via
    // eval_cb and the initial snapshot in attach_sse).  Do NOT call
    // bb_net_health_eval here — HTTP polls must not inject samples into the
    // hysteresis state.  Staleness is at most 5 s.
    bb_cache_serialize_into(BB_NET_HEALTH_TOPIC, section);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Telemetry source: publish the live net-health snapshot to bb_pub on the
// "net_health" subtopic. Registered here (not pulled in by bb_pub_health) so it
// exists ONLY when net_health is set up — bb_net_health already depends on
// bb_pub (adaptive backoff), so this adds no new dependency anywhere.
static bool net_health_pub_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_net_health_status_t ns;
    if (bb_net_health_get_status(&ns) != BB_OK) return false;  // not evaluated yet
    bb_net_health_emit(obj, &ns);
    return true;
}

void bb_net_health_register_health(void)
{
    bb_health_register_section("net", net_section_get, NULL, k_net_schema);
    bb_err_t perr = bb_pub_register_source("net_health", net_health_pub_sample, NULL);
    if (perr != BB_OK && perr != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "net_health pub source register failed: %d", (int)perr);
    }
}

bb_err_t bb_net_health_attach_sse(void)
{
    // Create the cache mutex before any path that reads or writes s_cache.
    s_cache_lock = xSemaphoreCreateMutex();
    if (!s_cache_lock) {
        bb_log_e(TAG, "cache lock create failed");
        return BB_ERR_NO_SPACE;
    }

    // Register net.health in bb_cache (owned-struct form).
    bb_err_t cerr = bb_cache_register(BB_NET_HEALTH_TOPIC, NULL,
                                      sizeof(bb_net_health_status_t),
                                      bb_net_health_emit);
    if (cerr != BB_OK) {
        bb_log_w(TAG, "bb_cache_register failed: %d", (int)cerr);
        return cerr;
    }

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

        // B1-362: capture disc_reason + tls_fail separately for publish_snapshot
        bb_mqtt_disc_t mqtt_disc_reason = BB_MQTT_DISC_NONE;
        bb_tls_fail_t  mqtt_tls_fail   = BB_TLS_FAIL_NONE;
        {
            bb_mqtt_t mqtt = bb_mqtt_default();
            if (mqtt) {
                bb_mqtt_stats_t stats;
                if (bb_mqtt_get_stats(mqtt, &stats) == BB_OK) {
                    mqtt_disc_reason = stats.disc_reason;
                    mqtt_tls_fail    = stats.tls_fail;
                }
            }
        }

        bb_sink_http_health_t http_h = {0};
        bb_sink_http_get_health(&http_h);

        publish_snapshot(&out, &in, &wi, false, mqtt_disc_reason, mqtt_tls_fail, http_h);
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
