// bb_net_health ESP-IDF glue — /api/diag/net evaluator.
//
// Call order:
//   bb_net_health_start()           — at PRE_HTTP tier
//
// The 5-second evaluator reads bb_wifi_get_info() and bb_mqtt_client_get_stats(), runs
// bb_net_health_eval against a static module state, and updates the cached
// snapshot read by bb_net_health_get_status() (served via /api/diag/net).
#include "bb_net_health.h"
#include "bb_board.h"
#include "bb_health.h"
#include "bb_mem.h"
#include "bb_wifi.h"
#include "bb_mqtt_client.h"
#include "bb_tls.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_timer.h"
#include "bb_clock.h"
#include "bb_transport_health.h"
#include "bb_str.h"
#include <inttypes.h>

#if CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE
// Persisted reboot-rate-limit state round-trips through bb_config (typed
// layer over bb_storage) rather than bb_nv's generic KV forwarder (B1-756,
// bb_nv dissolution epic B1-708) -- bb_config's STR encoding resolves to the
// SAME nvs_get_str/nvs_set_str calls bb_nv_get_str/set_str made (both are
// thin forwarders to bb_storage_nvs, see bb_storage_nvs.h), so the
// namespace/key/STR-typed on-flash format below is byte-compatible with what
// this component previously read/wrote via bb_nv.
#include "bb_config.h"
#include "bb_nv_namespaces.h"
#include "bb_nv_keys.h"
#include "bb_ntp.h"
#include "bb_system.h"
#include <time.h>
#endif

// Internal setter defined in components/bb_net_health/src/bb_net_health.c;
// not part of the public header (espidf-only call site).
extern void bb_net_health_set_heap_state(bb_heap_state_t state);

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "bb_net_health";

// Dedicated tag for the diagnostic-state log heartbeat (KB#556), distinct
// from TAG above, so an operator can raise/lower just the heartbeat via
// esp_log_level_set("net_state", ...) without touching other bb_net_health
// logs. The heartbeat is always compiled in; runtime level is the only
// on/off control (default INFO = visible).
static const char *LOG_TAG_NETSTATE = "net_state";

#define BB_NET_HEALTH_EVAL_PERIOD_US ((uint64_t)CONFIG_BB_NET_HEALTH_EVAL_PERIOD_S * 1000000ULL)

// Kconfig bridge (B1-518 PR3): mirrors the identical bridge in
// platform/espidf/bb_wifi/bb_wifi_gw_probe.c so this file uses the SAME
// consecutive-failure threshold the gw-probe worker itself arms
// gw_dead_count with — never a bare #ifndef alongside the CONFIG_ symbol.
#ifdef CONFIG_BB_WIFI_GW_PROBE_FAILS
#define BB_WIFI_GW_PROBE_FAILS CONFIG_BB_WIFI_GW_PROBE_FAILS
#endif
#ifndef BB_WIFI_GW_PROBE_FAILS
#define BB_WIFI_GW_PROBE_FAILS 3
#endif

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static bb_net_health_state_t s_state;       // hysteresis state (zero-init = valid)
static bb_wifi_link_state_t  s_last_published_state    = (bb_wifi_link_state_t)-1;
static bool                  s_last_published_warn     = false;
static bool                  s_last_published_throttled = false;
static bb_periodic_timer_t   s_timer = NULL;
// Diagnostic-state log heartbeat (KB#556): tracks the last time a heartbeat
// line was logged and the net_mode it was logged for, so bb_net_health_should_log
// can decide "immediate on transition, throttled otherwise". Sentinel -1
// mode guarantees the very first eval cycle always logs (mode transition
// from "never logged" to whatever the real mode is).
static int64_t                s_last_log_us   = 0;
static bb_wifi_mode_t         s_last_logged_mode = (bb_wifi_mode_t)-1;
// MQTT disconnect tracking: record the ms-timestamp when reconnect_count
// increases (the moment we detect a new disconnect).  0 means "no disconnect
// observed yet" (i.e. no reconnects have ever occurred).
static uint64_t              s_mqtt_disc_time_ms = 0;   // bb_timer_now_us()/1000 at last disc; _ms name is intentional (derived to mqtt_disc_age_s)
static uint32_t              s_last_reconnect_count = 0; // glue-side mirror for disc detection
// Egress-recovery SSOT (B1-518 PR3, OBSERVE-ONLY): last evaluator-cycle
// egress_state, used only to edge-trigger the "would recover" log line via
// bb_net_health_would_recover_edge. Zero-init = BB_EGRESS_STATE_OK, which is
// correct — no probe has run yet at boot.
static bb_egress_state_t     s_prev_egress_state = BB_EGRESS_STATE_OK;

#if CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE
// Egress-recovery ACT gate (B1-518 PR4). Single-writer via eval_work_fn (the
// evaluator timer callback) — same threading model as s_prev_egress_state
// above, no lock needed.
static uint32_t                     s_unhealthy_since_s   = 0;     // epoch-s; 0 = healthy
static bb_net_health_reboot_state_t s_reboot_state        = {0};   // zero-init valid
static bool                         s_reboot_state_loaded = false;

// Namespace/key byte-for-byte matched to bb_nv's prior
// BB_NET_HEALTH_EGRESS_ACT_NVS_NS ("bb_egress_act") /
// BB_NET_HEALTH_EGRESS_ACT_KEY_STATE ("state") — do not change without a
// migration plan, this strands provisioned-board egress-recovery
// reboot-rate-limit state otherwise.
static const bb_config_field_t s_egress_act_state_field = {
    .id          = "net_health.egress_act.state",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_NET_HEALTH_EGRESS_ACT_NVS_NS,
                      .key = BB_NET_HEALTH_EGRESS_ACT_KEY_STATE },
    .max_len     = BB_NET_HEALTH_REBOOT_STATE_STR_MAX,
    .def         = { .str = "" },
    .has_default = true,
};

// Load persisted reboot-rate-limit state from NVS. Idempotent; called once
// from bb_net_health_start() before the evaluator timer is armed, so
// eval_work_fn never races the load. Single-key packed string (B1-518 PR4
// finding MED) — one bb_config_get_str call, decoded via
// bb_net_health_reboot_state_decode. A missing key, first-boot empty
// fallback, or malformed/corrupt value all decode-fail safely into the
// existing zero-init s_reboot_state (no reboots recorded yet).
static void egress_act_load_state(void)
{
    if (s_reboot_state_loaded) {
        return;
    }
    memset(&s_reboot_state, 0, sizeof(s_reboot_state));

    char buf[BB_NET_HEALTH_REBOOT_STATE_STR_MAX];
    size_t out_len = 0;
    bb_err_t err = bb_config_get_str(&s_egress_act_state_field, buf, sizeof(buf), &out_len);
    // buf is stack-local and NOT zero-initialized (unlike bb_nv_get_str's
    // prior bb_strlcpy-based guarantee, bb_config_get_str's blob-fallback
    // path does not itself NUL-terminate) -- terminate explicitly based on
    // the reported length before treating buf as a C string, clamping to
    // the last byte on a genuine truncation (out_len >= cap).
    if (err == BB_OK) {
        size_t term = (out_len < sizeof(buf)) ? out_len : sizeof(buf) - 1;
        buf[term] = '\0';
    }
    if (err == BB_OK && buf[0] != '\0') {
        if (!bb_net_health_reboot_state_decode(buf, &s_reboot_state)) {
            bb_log_w(TAG, "egress ACT: corrupt persisted reboot state, resetting");
            memset(&s_reboot_state, 0, sizeof(s_reboot_state));
        }
    }

    s_reboot_state_loaded = true;
}

// Persist the current reboot-rate-limit state to NVS as a single packed
// string key (B1-518 PR4 finding MED — collapses ~13 sequential NVS commits
// down to 1). Called just before esp_restart() so the ring/last_reboot_s
// survive the reboot.
static void egress_act_persist_state(void)
{
    char buf[BB_NET_HEALTH_REBOOT_STATE_STR_MAX];
    if (!bb_net_health_reboot_state_encode(&s_reboot_state, buf, sizeof(buf))) {
        bb_log_w(TAG, "egress ACT: reboot state encode failed, not persisted");
        return;
    }
    bb_config_set_str(&s_egress_act_state_field, buf);
}
#endif // CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE

// Cached health snapshot — written only by eval_cb. /api/diag/net
// (bb_net_health_routes.c) reads this cache via bb_net_health_get_status
// instead of calling bb_net_health_eval, preventing HTTP polls from
// injecting extra samples into the hysteresis state.
typedef struct {
    int8_t   rssi;
    bool     mqtt_connected;
    uint32_t mqtt_reconnect_count;
    uint32_t last_disconnect_reason;
    uint32_t disc_age_s;
    bb_wifi_link_state_t state;
    bool     early_warning;
    bool     throttled;
    uint32_t mqtt_disc_age_s;
    uint32_t mqtt_disc_reason;
    uint32_t mqtt_tls_fail;
    uint32_t lost_ip_recoveries;
    uint32_t lost_ip_age_s;
    uint32_t egress_dead_recoveries;
    uint32_t no_ip_recoveries; // B1-486 finding #4: captured alongside lost_ip/egress_dead
    uint32_t roam_count;       // B1-497: observe-only, captured alongside the recovery counters
    uint32_t roam_age_s;
    uint32_t last_session_s;   // duration of the most recently ended connected session (observe-only)
    bb_wifi_mode_t net_mode;    // WiFi discrimination mode (observe-only)
    bool     associated;
    bool     has_ip;
    bool     gw_available;    // B1-518 PR3: gateway-probe status (observe-only)
    bool     gw_reachable;
    uint8_t  gw_fail_streak;
    uint32_t gw_dead_count;
    uint64_t last_gw_probe_ms;
    bool     tx_available;    // B1-518 PR2: bb_transport_health cached counts (observe-only)
    int      tx_enabled;
    int      tx_failing;
    bb_egress_state_t egress_state; // B1-518 PR3: arbiter verdict (observe-only)
} bb_net_health_cache_t;

static bb_net_health_cache_t s_cache;       // zero-init; valid once eval_work_fn writes it
static SemaphoreHandle_t     s_cache_lock;  // protects s_cache against torn read/write

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

    bb_mqtt_client_t mqtt = bb_mqtt_client_default();
    if (mqtt) {
        bb_mqtt_client_stats_t stats;
        if (bb_mqtt_client_get_stats(mqtt, &stats) == BB_OK) {
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

// Pull the gateway-probe worker's last observed state (B1-518 PR3,
// OBSERVE-ONLY). *out_available is true only once at least one probe has
// actually completed: bb_wifi_get_gateway_status returns BB_ERR_INVALID_STATE
// when the worker never started (CONFIG_BB_WIFI_GW_PROBE_ENABLE=n), but a
// BB_OK zeroed status (last_gw_probe_ms == 0) is also possible in the window
// between worker start and the first probe firing — treat both as
// unavailable so callers never fabricate a false "gw reachable" reading.
static void pull_gw_status(bool *out_available, bb_wifi_gw_status_t *out_gw)
{
    memset(out_gw, 0, sizeof(*out_gw));
    *out_available = (bb_wifi_get_gateway_status(out_gw) == BB_OK) &&
                      (out_gw->last_gw_probe_ms > 0);
}

// Pull the bb_transport_health cached AUTHORITATIVE counts (B1-518 PR2,
// OBSERVE-ONLY). *out_available is true only when the call succeeds AND at
// least one AUTHORITATIVE transport is currently enabled — mirrors
// pull_gw_status's "never fabricate a false reading" contract: a board with
// no registered transport (or the exclusive-sink arbiter having disabled
// them all) reports tx_available=false rather than a misleading 0/0.
static void pull_tx_status(bool *out_available, int *out_enabled, int *out_failing)
{
    int enabled = 0, failing = 0;
    bb_err_t rc = bb_transport_health_authoritative_counts(&enabled, &failing);
    *out_available = (rc == BB_OK) && (enabled > 0);
    *out_enabled   = enabled;
    *out_failing   = failing;
}

// Return the cached snapshot last written by publish_snapshot(). The full
// detail is always available via GET /api/diag/net (bb_net_health_routes.c),
// which reads from the cache that is updated unconditionally there.
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
    out->gw_available               = s_cache.gw_available;
    out->gw_reachable               = s_cache.gw_reachable;
    out->gw_fail_streak             = s_cache.gw_fail_streak;
    out->gw_dead_count              = s_cache.gw_dead_count;
    out->last_gw_probe_ms           = s_cache.last_gw_probe_ms;
    out->tx_available               = s_cache.tx_available;
    out->tx_enabled                 = s_cache.tx_enabled;
    out->tx_failing                 = s_cache.tx_failing;
    out->egress_state               = s_cache.egress_state;
    xSemaphoreGive(s_cache_lock);
    return BB_OK;
}

// Build a full status snapshot from the current evaluation inputs/outputs.
// Pure construction — no cache writes. Called once per eval_work_fn cycle so
// both the publish path (on state/warning/throttle change) and the
// log-heartbeat path (on net_mode change or interval elapsed) observe the
// exact same point-in-time snapshot.
static bb_net_health_status_t build_snapshot(const bb_net_health_output_t *out,
                                             const bb_net_health_input_t  *in,
                                             const bb_wifi_info_t         *wi,
                                             bool throttled,
                                             bb_mqtt_client_disc_t mqtt_disc_reason,
                                             bb_tls_fail_t  mqtt_tls_fail,
                                             bool gw_available,
                                             const bb_wifi_gw_status_t *gw,
                                             bool tx_available,
                                             int tx_enabled,
                                             int tx_failing,
                                             bb_egress_state_t egress_state)
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
        .lost_ip_recoveries     = bb_wifi_get_lost_ip_count(),
        .lost_ip_age_s          = bb_wifi_get_lost_ip_age_s(),
        .egress_dead_recoveries = bb_wifi_get_egress_dead_count(),
        .no_ip_recoveries       = bb_wifi_get_no_ip_count(),
        .roam_count             = bb_wifi_get_roam_count(),
        .roam_age_s             = bb_wifi_get_roam_age_s(),
        .last_session_s         = bb_wifi_get_last_session_s(),
        .net_mode               = bb_wifi_classify_mode(associated, has_ip),
        .associated             = associated,
        .has_ip                 = has_ip,
        .retry_count            = wi->retry_count,
        .restart_sta_count      = bb_wifi_get_restart_sta_count(),
        .uptime_s               = (uint32_t)(bb_clock_now_ms64() / 1000ULL),
        .gw_available           = gw_available,
        .gw_reachable           = gw->gw_reachable,
        .gw_fail_streak         = gw->gw_fail_streak,
        .gw_dead_count          = gw->gw_dead_count,
        .last_gw_probe_ms       = gw->last_gw_probe_ms,
        .tx_available           = tx_available,
        .tx_enabled             = tx_enabled,
        .tx_failing             = tx_failing,
        .egress_state           = egress_state,
    };
    bb_strlcpy(snap.ip, wi->ip, sizeof(snap.ip));

    return snap;
}

// Update s_cache from an already-built snapshot.
static void publish_snapshot(const bb_net_health_status_t *snap)
{
    // Update s_cache so bb_net_health_get_status (used by GET /api/diag/net)
    // can read the latest snapshot without re-running eval.
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
    s_cache.gw_available            = snap->gw_available;
    s_cache.gw_reachable            = snap->gw_reachable;
    s_cache.gw_fail_streak          = snap->gw_fail_streak;
    s_cache.gw_dead_count           = snap->gw_dead_count;
    s_cache.last_gw_probe_ms        = snap->last_gw_probe_ms;
    s_cache.tx_available            = snap->tx_available;
    s_cache.tx_enabled              = snap->tx_enabled;
    s_cache.tx_failing              = snap->tx_failing;
    s_cache.egress_state            = snap->egress_state;
    xSemaphoreGive(s_cache_lock);
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
    bb_mqtt_client_disc_t mqtt_disc_reason = BB_MQTT_CLIENT_DISC_NONE;
    bb_tls_fail_t  mqtt_tls_fail   = BB_TLS_FAIL_NONE;
    {
        bb_mqtt_client_t mqtt = bb_mqtt_client_default();
        if (mqtt) {
            bb_mqtt_client_stats_t stats;
            if (bb_mqtt_client_get_stats(mqtt, &stats) == BB_OK) {
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

    bool currently_throttled = false;

    bool gw_available;
    bb_wifi_gw_status_t gw;
    pull_gw_status(&gw_available, &gw);

    bool tx_available;
    int  tx_enabled, tx_failing;
    pull_tx_status(&tx_available, &tx_enabled, &tx_failing);

    // Egress-recovery SSOT (B1-518 PR3, OBSERVE-ONLY): net_mode is computed
    // here (not read back from the snapshot below) so the classify_egress
    // call and build_snapshot's internal net_mode recompute observe the
    // same live bb_wifi_is_associated()/bb_wifi_has_ip() read pair.
    bb_wifi_mode_t egress_wifi_mode =
        bb_wifi_classify_mode(bb_wifi_is_associated(), bb_wifi_has_ip());
    bb_egress_state_t egress_state =
        bb_net_health_classify_egress(egress_wifi_mode, gw_available, gw.gw_reachable,
                                       gw.gw_fail_streak, (uint8_t)BB_WIFI_GW_PROBE_FAILS,
                                       tx_enabled, tx_failing);

    // Build the full snapshot once per cycle: both the SSE-publish decision
    // below and the log-heartbeat decision (KB#556) observe the same
    // point-in-time snapshot.
    bb_net_health_status_t snap = build_snapshot(&out, &in, &wi, currently_throttled,
                                                  mqtt_disc_reason, mqtt_tls_fail,
                                                  gw_available, &gw,
                                                  tx_available, tx_enabled, tx_failing,
                                                  egress_state);

    // Would-recover log (B1-518 PR3, observe-only, edge-triggered): fires
    // once when egress_state transitions INTO GW_UNREACHABLE — the only
    // state that would trigger a WiFi restart under a future act gate.  No
    // recovery action is taken here; this is a diagnostic breadcrumb only.
    if (bb_net_health_would_recover_edge(s_prev_egress_state, egress_state)) {
        bb_log_w(TAG, "would recover: egress failing + gateway unreachable (act disabled)");
    }

#if CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE
    // Egress-recovery ACT gate (B1-518 PR4): tier-2 app-driven recovery
    // request (UNGATED — edge-triggered, needs no epoch time) + tier-3
    // rate-limited reboot escalation (GATED on NTP sync — see below). All
    // state here is single-writer (this evaluator timer callback);
    // esp_restart() and NVS access are confined entirely to this #if block.
    {
        // Tier-2: request app-driven WiFi recovery on the edge into
        // GW_UNREACHABLE. act_enabled is always true here — the whole block
        // is compile-gated — but the predicate is still called (not
        // inlined) so the decision logic is centralized and host-tested.
        // Deliberately OUTSIDE the sync gate below: it is edge-triggered off
        // egress_state transitions and never touches now_s/epoch time.
        if (bb_net_health_should_request_recovery(s_prev_egress_state, egress_state, true)) {
            bb_wifi_request_recovery("egress: gw unreachable");
        }

        // Tier-3: gated on a valid wall clock. now_s MUST be epoch seconds
        // (time(NULL)), never a boot-relative monotonic uptime — an
        // uptime-derived value resets to 0 across esp_restart() and would
        // silently defeat the persisted 24h daily-cap / min-interval rate
        // limits, reopening a reboot-loop risk (the bug this gate fixes).
        // When the clock has never synced, tier-3 is LOG-ONLY: no
        // esp_restart(), no NVS persist, no ring update — a never-synced
        // board simply never tier-3-reboots, the safe direction.
        bool synced = bb_ntp_is_synced();
        uint32_t now_s = synced ? (uint32_t)time(NULL) : 0U;

        // Track the sustained-unhealthy window: arm on the transition INTO
        // GW_UNREACHABLE (only once the clock is synced — an unsynced arm
        // would record a bogus epoch-0-relative "since"), clear on
        // transition OUT. If we're already in GW_UNREACHABLE when the clock
        // becomes synced (unhealthy_since_s still 0), arm it now so the
        // T_reboot window starts counting from the first synced tick rather
        // than never arming until the next fresh transition.
        if (egress_state == BB_EGRESS_STATE_GW_UNREACHABLE) {
            if (synced && s_unhealthy_since_s == 0) {
                s_unhealthy_since_s = now_s;
            }
            if (!synced && s_prev_egress_state != BB_EGRESS_STATE_GW_UNREACHABLE) {
                bb_log_w(TAG, "egress ACT: tier-3 deferred: clock not synced");
            }
        } else {
            s_unhealthy_since_s = 0;
        }

        if (synced) {
            // Tier-3: rate-limited reboot escalation.
            if (bb_net_health_should_reboot(s_unhealthy_since_s, now_s,
                                             (uint32_t)CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_S,
                                             (uint32_t)CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_MIN_INTERVAL_S,
                                             (uint32_t)CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_DAILY_CAP,
                                             &s_reboot_state)) {
                bb_net_health_reboot_state_record(&s_reboot_state, now_s);
                egress_act_persist_state();

                uint8_t count24h = 0;
                uint8_t n = s_reboot_state.ring_count;
                if (n > BB_NET_HEALTH_REBOOT_CAP_MAX) n = BB_NET_HEALTH_REBOOT_CAP_MAX;
                for (uint8_t i = 0; i < n; i++) {
                    uint32_t ts = s_reboot_state.reboot_s_ring[i];
                    if (now_s >= ts && (now_s - ts) < 86400U) count24h++;
                }

                bb_log_w(TAG, "egress ACT: sustained gw_unreachable >= %" PRIu32
                              "s, reboot %u/%u in 24h - restarting",
                         (uint32_t)CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_S,
                         (unsigned)count24h,
                         (unsigned)CONFIG_BB_NET_HEALTH_EGRESS_ACT_REBOOT_DAILY_CAP);
                bb_system_restart_reason(BB_RESET_SRC_EGRESS_TIER3, "gw unreachable");
            }
        }
    }
#endif // CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE

    s_prev_egress_state = egress_state;

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

    // Diagnostic-state log heartbeat (KB#556, observe-only): emit a
    // structured line on every net_mode transition (immediate) or every
    // BB_NET_HEALTH_LOG_INTERVAL_S seconds (throttled periodic heartbeat),
    // independent of whether the cache snapshot above published. Rides the
    // "log" bb_event topic (serial + future UDP log sink) so a no-route/
    // zombie board that cannot serve HTTP/MQTT still reports full state.
    // Always compiled in; runtime-controlled via the "net_state" tag's log
    // level. esp_log_level_get() gates the format_log snprintf work so a
    // suppressed heartbeat (tag level below INFO) costs ~nothing beyond the
    // should_log rate-limit check.
    int64_t now_us = bb_timer_now_us();
    if (bb_net_health_should_log(now_us, s_last_log_us, snap.net_mode,
                                  s_last_logged_mode,
                                  (uint32_t)BB_NET_HEALTH_LOG_INTERVAL_S)) {
        s_last_log_us      = now_us;
        s_last_logged_mode = snap.net_mode;
        if (esp_log_level_get(LOG_TAG_NETSTATE) >= ESP_LOG_INFO) {
            char line[224];
            bb_net_health_format_log(&snap, line, sizeof(line));
            bb_log_i(LOG_TAG_NETSTATE, "%s", line);
        }
    }
}

// ---------------------------------------------------------------------------
// PRE_HTTP lifecycle entry point
// ---------------------------------------------------------------------------

// Starts the background evaluator: cache mutex + deferred periodic timer.
// The mutex MUST be created before the timer is armed: publish_snapshot
// (called from eval_work_fn on the timer) does an unguarded xSemaphoreTake
// on s_cache_lock, so an armed timer racing ahead of mutex creation is a
// NULL-handle take -> hard fault.
bb_err_t bb_net_health_start(void)
{
    s_cache_lock = xSemaphoreCreateMutex();
    if (!s_cache_lock) {
        bb_log_e(TAG, "cache lock create failed");
        return BB_ERR_NO_SPACE;
    }

#if CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE
    // Load persisted reboot-rate-limit state before the evaluator timer is
    // armed, so eval_work_fn never races the load.
    egress_act_load_state();
#endif

    bb_err_t err = bb_timer_deferred_periodic_create(eval_work_fn, NULL, "bb_net_health", &s_timer);
    if (err != BB_OK) {
        bb_log_e(TAG, "timer create failed: %d", (int)err);
        return err;
    }
    err = bb_timer_periodic_start(s_timer, BB_NET_HEALTH_EVAL_PERIOD_US);
    if (err != BB_OK) {
        bb_log_e(TAG, "timer start failed: %d", (int)err);
        return err;
    }

    bb_log_i(TAG, "evaluator started");
    return BB_OK;
}
