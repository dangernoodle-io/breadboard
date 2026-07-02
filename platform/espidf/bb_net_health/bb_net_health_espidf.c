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
#include "bb_board.h"
#include "bb_cache.h"
#include "bb_health.h"
#include "bb_mem.h"
#include "bb_openapi.h"
#include "bb_wifi.h"
#include "bb_mqtt.h"
#include "bb_tls.h"
#include "bb_sink_http.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_timer.h"
#include "bb_clock.h"
#include "bb_pub.h"
#include <inttypes.h>

// Internal setter defined in components/bb_net_health/src/bb_net_health.c;
// not part of the public header (espidf-only call site).
extern void bb_net_health_set_heap_state(bb_heap_state_t state);

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
// Diagnostic-state log heartbeat (KB#556): tracks the last time a heartbeat
// line was logged and the net_mode it was logged for, so bb_net_health_should_log
// can decide "immediate on transition, throttled otherwise". Sentinel -1
// mode guarantees the very first eval cycle always logs (mode transition
// from "never logged" to whatever the real mode is).
static int64_t                s_last_log_us   = 0;
static bb_net_mode_t          s_last_logged_mode = (bb_net_mode_t)-1;
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
    uint32_t lost_ip_recoveries;
    uint32_t lost_ip_age_s;
    uint32_t egress_dead_recoveries;
    uint32_t no_ip_recoveries; // B1-486 finding #4: captured alongside lost_ip/egress_dead
    uint32_t roam_count;       // B1-497: observe-only, captured alongside the recovery counters
    uint32_t roam_age_s;
    uint32_t last_session_s;   // duration of the most recently ended connected session (observe-only)
    bb_net_mode_t net_mode;    // WiFi discrimination mode (observe-only)
    bool     associated;
    bool     has_ip;
} bb_net_health_cache_t;

static bb_net_health_cache_t s_cache;       // zero-init; valid once attach_sse writes it
static SemaphoreHandle_t     s_cache_lock;  // protects s_cache against torn read/write

// JSON-Schema for the "net" health section — status bools/enums only (TA-505).
// Numeric counters moved to GET /api/diag/net.
static const char k_net_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"state\":{\"type\":\"string\"},"
    "\"early_warning\":{\"type\":\"boolean\"},"
    "\"throttled\":{\"type\":\"boolean\"},"
    "\"mqtt\":{\"type\":\"object\",\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"}"
    "}},"
    "\"http\":{\"type\":\"object\",\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"}"
    "}}}}";

// Full schema for the net.health SSE topic — matches bb_net_health_emit output.
// (Status-only k_net_schema is used only for the /api/health "net" section.)
static const char k_net_sse_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"state\":{\"type\":\"string\"},"
    "\"early_warning\":{\"type\":\"boolean\"},"
    "\"throttled\":{\"type\":\"boolean\"},"
    "\"last_disconnect_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"lost_ip_recoveries\":{\"type\":\"integer\"},"
    "\"lost_ip_age_s\":{\"type\":\"integer\"},"
    "\"egress_dead_recoveries\":{\"type\":\"integer\"},"
    "\"mqtt\":{\"type\":\"object\",\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"reconnect_count\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"tls_fail\":{\"type\":\"integer\"}}},"
    "\"http\":{\"type\":\"object\",\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"consec_failures\":{\"type\":\"integer\"},"
    "\"tls_fail\":{\"type\":\"integer\"},"
    "\"last_status\":{\"type\":\"integer\"}}},"
    "\"no_ip_recoveries\":{\"type\":\"integer\"},"
    "\"roam_count\":{\"type\":\"integer\"},"
    "\"roam_age_s\":{\"type\":\"integer\"},"
    "\"last_session_s\":{\"type\":\"integer\"},"
    "\"net_mode\":{\"type\":\"string\"},"
    "\"associated\":{\"type\":\"boolean\"},"
    "\"has_ip\":{\"type\":\"boolean\"}}}";

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
    out->lost_ip_recoveries         = s_cache.lost_ip_recoveries;
    out->lost_ip_age_s              = s_cache.lost_ip_age_s;
    out->egress_dead_recoveries     = s_cache.egress_dead_recoveries;
    out->no_ip_recoveries           = s_cache.no_ip_recoveries;
    out->roam_count                 = s_cache.roam_count;
    out->roam_age_s                 = s_cache.roam_age_s;
    out->last_session_s             = s_cache.last_session_s;
    out->net_mode                   = s_cache.net_mode;
    out->associated                 = s_cache.associated;
    out->has_ip                     = s_cache.has_ip;
    xSemaphoreGive(s_cache_lock);
    return BB_OK;
}

// Build a full status snapshot from the current evaluation inputs/outputs.
// Pure construction — no cache writes, no SSE/bb_cache posts. Called once per
// eval_work_fn cycle so both the SSE-publish path (on state/warning/throttle
// change) and the log-heartbeat path (on net_mode change or interval
// elapsed) observe the exact same point-in-time snapshot.
static bb_net_health_status_t build_snapshot(const bb_net_health_output_t *out,
                                             const bb_net_health_input_t  *in,
                                             const bb_wifi_info_t         *wi,
                                             bool throttled,
                                             bb_mqtt_disc_t mqtt_disc_reason,
                                             bb_tls_fail_t  mqtt_tls_fail,
                                             bb_sink_http_health_t http_h)
{
    bool associated = bb_wifi_is_associated();
    bool has_ip     = bb_wifi_has_ip();

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
        .lost_ip_recoveries     = bb_wifi_get_lost_ip_count(),
        .lost_ip_age_s          = bb_wifi_get_lost_ip_age_s(),
        .egress_dead_recoveries = bb_wifi_get_egress_dead_count(),
        .no_ip_recoveries       = bb_wifi_get_no_ip_count(),
        .roam_count             = bb_wifi_get_roam_count(),
        .roam_age_s             = bb_wifi_get_roam_age_s(),
        .last_session_s         = bb_wifi_get_last_session_s(),
        .net_mode               = bb_net_health_classify_mode(associated, has_ip),
        .associated             = associated,
        .has_ip                 = has_ip,
        .retry_count            = wi->retry_count,
        .restart_sta_count      = bb_wifi_get_restart_sta_count(),
        .uptime_s               = (uint32_t)(bb_clock_now_ms64() / 1000ULL),
    };
    strncpy(snap.ip, wi->ip, sizeof(snap.ip) - 1);
    snap.ip[sizeof(snap.ip) - 1] = '\0';

    return snap;
}

// Update s_cache + bb_cache/SSE from an already-built snapshot.
static void publish_snapshot(const bb_net_health_status_t *snap)
{
    // Update s_cache so bb_net_health_get_status (used by the pub source) can
    // read the latest snapshot without re-running eval.
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    s_cache.rssi                    = snap->rssi;
    s_cache.mqtt_connected          = snap->mqtt_connected;
    s_cache.mqtt_reconnect_count    = snap->mqtt_reconnect_count;
    s_cache.last_disconnect_reason  = snap->last_disconnect_reason;
    s_cache.disc_age_s              = snap->disc_age_s;
    s_cache.state                   = snap->state;
    s_cache.early_warning           = snap->early_warning;
    s_cache.throttled               = snap->throttled;
    s_cache.mqtt_disc_age_s         = snap->mqtt_disc_age_s;
    s_cache.mqtt_disc_reason        = snap->mqtt_disc_reason;
    s_cache.mqtt_tls_fail           = snap->mqtt_tls_fail;
    s_cache.http_connected          = snap->http_connected;
    s_cache.http_consec_failures    = snap->http_consec_failures;
    s_cache.http_tls_fail           = snap->http_tls_fail;
    s_cache.http_last_status        = snap->http_last_status;
    s_cache.lost_ip_recoveries      = snap->lost_ip_recoveries;
    s_cache.lost_ip_age_s           = snap->lost_ip_age_s;
    s_cache.egress_dead_recoveries  = snap->egress_dead_recoveries;
    s_cache.no_ip_recoveries        = snap->no_ip_recoveries;
    s_cache.roam_count              = snap->roam_count;
    s_cache.roam_age_s              = snap->roam_age_s;
    s_cache.last_session_s          = snap->last_session_s;
    s_cache.net_mode                = snap->net_mode;
    s_cache.associated              = snap->associated;
    s_cache.has_ip                  = snap->has_ip;
    xSemaphoreGive(s_cache_lock);

    // Update bb_cache owned struct — SSE uses bb_cache_post, REST uses
    // bb_cache_serialize_into; both serialize from the same snapshot.
    bb_cache_update(BB_NET_HEALTH_TOPIC, snap);
    bb_cache_post(BB_NET_HEALTH_TOPIC);
}

// ---------------------------------------------------------------------------
// Evaluator timer callback (fires every 5 s on ESP-IDF)
// ---------------------------------------------------------------------------

static void eval_work_fn(void *arg)
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

    // Heap state: always update the module-static (read by bb_net_health_heap_state).
    // g_free is the only heap_caps walk on the unconditional path; g_min and
    // g_largest are only needed for the HEAPTRACE log line, so they stay inside
    // that block to avoid three concurrent heap_caps walks on every tick on
    // single-core / low-RAM targets like the C3 (B1-433).
    {
        size_t g_free = bb_board_heap_free_total();
        bb_heap_state_t heap_st = bb_net_health_classify_heap(g_free);
        bb_net_health_set_heap_state(heap_st);

#if BB_NET_HEALTH_HEAP_TRACE
        size_t g_min     = bb_board_heap_minimum_ever();
        size_t g_largest = bb_board_heap_largest_free_block();
        bb_mem_stats_t ms;
        bb_mem_get_stats(&ms);
        bb_log_i(TAG,
                 "HEAPTRACE up=%" PRIu32
                 " g_free=%zu g_min=%zu g_largest=%zu state=%s"
                 " m_out=%zu m_peak=%zu"
                 " m_allocs=%" PRIu32 " m_frees=%" PRIu32 " m_fail=%" PRIu32
                 " m_sp=%zu m_int=%zu",
                 bb_clock_now_ms(),
                 g_free, g_min, g_largest,
                 bb_heap_state_str(heap_st),
                 ms.outstanding_bytes, ms.peak_outstanding,
                 ms.alloc_count, ms.free_count, ms.alloc_fail,
                 ms.spiram_alloc_bytes, ms.internal_alloc_bytes);
#endif
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

    bb_sink_http_health_t http_h = {0};
    bb_sink_http_get_health(&http_h);

    // Build the full snapshot once per cycle: both the SSE-publish decision
    // below and the log-heartbeat decision (KB#556) observe the same
    // point-in-time snapshot.
    bb_net_health_status_t snap = build_snapshot(&out, &in, &wi, currently_throttled,
                                                  mqtt_disc_reason, mqtt_tls_fail, http_h);

    // Publish only when state, early_warning, OR throttled flag differs from
    // the last published values.  This ensures:
    //  - A sustained throttle publishes ONCE on entry and ONCE on exit.
    //  - Every real state/warning transition is still captured.
    if (out.state != s_last_published_state ||
        out.early_warning != s_last_published_warn ||
        currently_throttled != s_last_published_throttled) {
        publish_snapshot(&snap);
        s_last_published_state     = out.state;
        s_last_published_warn      = out.early_warning;
        s_last_published_throttled = currently_throttled;
    }

#if BB_NET_HEALTH_LOG_ENABLE
    // Diagnostic-state log heartbeat (KB#556, observe-only): emit a
    // structured line on every net_mode transition (immediate) or every
    // BB_NET_HEALTH_LOG_INTERVAL_S seconds (throttled periodic heartbeat),
    // independent of whether the SSE snapshot above published. Rides the
    // "log" bb_event topic (serial + future UDP log sink) so a no-route/
    // zombie board that cannot serve HTTP/MQTT still reports full state.
    int64_t now_us = bb_timer_now_us();
    if (bb_net_health_should_log(now_us, s_last_log_us, snap.net_mode,
                                  s_last_logged_mode,
                                  (uint32_t)BB_NET_HEALTH_LOG_INTERVAL_S)) {
        char line[224];
        bb_net_health_format_log(&snap, line, sizeof(line));
        bb_log_i(TAG, "%s", line);
        s_last_log_us      = now_us;
        s_last_logged_mode = snap.net_mode;
    }
#endif
}

// ---------------------------------------------------------------------------
// /api/health "net" section callback
// ---------------------------------------------------------------------------

static void net_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    // Emit status-only (bools/enums) — numerics moved to /api/diag/net (TA-505).
    // Read from the cached snapshot under lock; do NOT call bb_net_health_eval
    // here — HTTP polls must not inject samples into the hysteresis state.
    bb_net_health_status_t snap;
    if (bb_net_health_get_status(&snap) == BB_OK) {
        bb_net_health_emit_status(section, &snap);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Telemetry source: publish the live net-health snapshot to bb_pub on the
// "net_health" subtopic. Registered here (not pulled in by bb_pub_health) so it
// exists ONLY when net_health is set up — bb_net_health already depends on
// bb_pub (adaptive backoff), so this adds no new dependency anywhere.
//
// Migration (telemetry-ssot): uses bb_pub_register_telemetry so the snapshot
// is gathered into bb_cache once per tick; SSE and sinks all read the SAME
// memoized serialization.  ts_ms is stamped at gather time.
//
// Topic name: "net_health" (distinct from the state topic "net.health" /
// BB_NET_HEALTH_TOPIC — no collision).  The existing net.health bb_cache +
// SSE path (bb_net_health_attach_sse) is 100% intact and unchanged.
//
// SSE routing: bb_pub_register_telemetry with BB_PUB_TELEM_SSE registers the
// bb_cache event topic automatically (via BB_CACHE_FLAG_SSE).  Phase-2b of
// bb_pub_tick_once calls bb_cache_post_serialized to post to that topic.  No
// explicit bb_event_routes_attach call is needed — the same pattern used by
// the "wifi" telem topic.

typedef struct {
    bb_net_health_status_t status;
    int64_t                ts_ms;
} bb_net_health_snap_t;

// Compile-time guard: net_health snap must fit in the scratch buffer (B1-434).
// CONFIG_BB_PUB_TELEM_SNAP_MAX is always available here (ESP-IDF only file).
typedef char _net_health_snap_size_check[
    sizeof(bb_net_health_snap_t) <= CONFIG_BB_PUB_TELEM_SNAP_MAX ? 1 : -1];

static bool net_health_gather(void *snap_buf, void *ctx)
{
    (void)ctx;
    bb_net_health_snap_t *s = snap_buf;
    memset(s, 0, sizeof(*s));
    if (bb_net_health_get_status(&s->status) != BB_OK) return false;  // not evaluated yet
    s->ts_ms = (int64_t)bb_clock_now_ms64();
    return true;
}

static void net_health_serialize(bb_json_t obj, const void *snap_raw)
{
    const bb_net_health_snap_t *s = snap_raw;
    bb_net_health_emit(obj, &s->status);
    bb_json_obj_set_int(obj, "ts_ms", s->ts_ms);
}

void bb_net_health_register_health(void)
{
    bb_health_register_section("net", net_section_get, NULL, k_net_schema);

    bb_pub_telemetry_cfg_t cfg = {
        .topic     = "net_health",
        .gather    = net_health_gather,
        .serialize = net_health_serialize,
        .snap_size = sizeof(bb_net_health_snap_t),
        .flags     = BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS,
        .ctx       = NULL,
    };
    bb_err_t perr = bb_pub_register_telemetry(&cfg);
    if (perr != BB_OK && perr != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "net_health register_telemetry failed: %d", (int)perr);
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

    bb_openapi_register_topic_schema(BB_NET_HEALTH_TOPIC, k_net_sse_schema, "NetHealth");

    // Register the event topic so bb_event_post can target it.
    bb_err_t err = bb_event_topic_register(BB_NET_HEALTH_TOPIC, &s_topic);
    if (err != BB_OK) {
        bb_log_w(TAG, "topic register failed: %d", (int)err);
        return err;
    }

    // Attach as a retained SSE topic. max_entry=BB_NET_HEALTH_SSE_MAX_ENTRY
    // (512, defined in bb_net_health.h): the serialized snapshot (nested
    // mqtt/http objects) measures ~341 B on HW (~352 B in the host test's
    // synthetic worst-ish case — the gap is digit-width in a few integer
    // fields, not a real discrepancy), above the bb_event_routes global
    // default (256) — same precedent as update.available / info.build
    // (#616, B1-434/435/439; see bb_ota_check_espidf.c / bb_info.c).
    // B1-472.
    err = bb_event_routes_attach_ex2(BB_NET_HEALTH_TOPIC, /*retained=*/true,
                                      BB_NET_HEALTH_SSE_MAX_ENTRY);
    if (err != BB_OK) {
        bb_log_w(TAG, "attach_ex2 failed: %d", (int)err);
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

        bb_net_health_status_t snap = build_snapshot(&out, &in, &wi, false,
                                                      mqtt_disc_reason, mqtt_tls_fail, http_h);
        publish_snapshot(&snap);
        s_last_published_state     = out.state;
        s_last_published_warn      = out.early_warning;
        s_last_published_throttled = false;

#if BB_NET_HEALTH_LOG_ENABLE
        // Initial heartbeat: sentinel s_last_logged_mode guarantees this
        // always logs (mode transition from "never logged"), so a
        // no-route/zombie board reports diagnostic state from T=0.
        int64_t now_us = bb_timer_now_us();
        char line[224];
        bb_net_health_format_log(&snap, line, sizeof(line));
        bb_log_i(TAG, "%s", line);
        s_last_log_us      = now_us;
        s_last_logged_mode = snap.net_mode;
#endif
    }

    // Start the 5-second periodic evaluator.
    err = bb_timer_deferred_periodic_create(eval_work_fn, NULL, "bb_net_health", &s_timer);
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
