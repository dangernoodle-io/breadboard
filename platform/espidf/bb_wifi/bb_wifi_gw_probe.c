// Gateway-reachability probe worker (observe-only, B1-518 PR2).
//
// Runs bb_wifi_gateway_reachable() on a dedicated bb_timer worker task
// (MODE B) — NOT the shared bb_timer_disp task. bb_wifi_ping blocks up to
// timeout_ms + 500ms; running that on the shared deferred-timer task would
// stall every other light-housekeeping consumer.
//
// OBSERVE-ONLY (#578 removed the old FSM-embedded probe deliberately: a bare
// gateway ping must NOT trigger recovery on its own). The work_fn feeds the
// ping result into the pure wifi_reconn_policy_on_egress_probe() classifier
// using a SEPARATE, observe-owned wifi_reconn_state_t — never the live FSM
// state owned by wifi_reconn.c. The returned wifi_reconn_action_t is used
// ONLY to increment gw_dead_count (a would-have-tripped counter) and is
// otherwise DISCARDED. No bb_wifi_restart_sta / bb_wifi_request_recovery /
// esp_wifi_* call is ever made from this file. This discard IS the
// observe/act boundary. bb_net_health (a later PR) pulls the recorded status
// via bb_wifi_get_gateway_status(); the act gate is a separate, later PR.
#include "bb_wifi.h"
#include "wifi_reconn_policy.h"
#include "bb_log.h"
#include "bb_timer.h"
#include "bb_clock.h"

#include "freertos/FreeRTOS.h"

static const char *TAG = "bb_wifi_gw_probe";

// --- Kconfig bridge (CONFIG_BB_WIFI_GW_PROBE_* -> BB_WIFI_GW_PROBE_*) ---
// Never a bare #ifndef alongside the CONFIG_ symbol — that shadows the
// generated Kconfig value and silently makes the knob inert.
#ifdef CONFIG_BB_WIFI_GW_PROBE_PERIOD_S
#define BB_WIFI_GW_PROBE_PERIOD_S CONFIG_BB_WIFI_GW_PROBE_PERIOD_S
#endif
#ifndef BB_WIFI_GW_PROBE_PERIOD_S
#define BB_WIFI_GW_PROBE_PERIOD_S 20
#endif

#ifdef CONFIG_BB_WIFI_GW_PROBE_TIMEOUT_MS
#define BB_WIFI_GW_PROBE_TIMEOUT_MS CONFIG_BB_WIFI_GW_PROBE_TIMEOUT_MS
#endif
#ifndef BB_WIFI_GW_PROBE_TIMEOUT_MS
#define BB_WIFI_GW_PROBE_TIMEOUT_MS 1000
#endif

#ifdef CONFIG_BB_WIFI_GW_PROBE_FAILS
#define BB_WIFI_GW_PROBE_FAILS CONFIG_BB_WIFI_GW_PROBE_FAILS
#endif
#ifndef BB_WIFI_GW_PROBE_FAILS
#define BB_WIFI_GW_PROBE_FAILS 3
#endif

#ifdef CONFIG_BB_WIFI_GW_PROBE_STACK
#define BB_WIFI_GW_PROBE_STACK CONFIG_BB_WIFI_GW_PROBE_STACK
#endif
#ifndef BB_WIFI_GW_PROBE_STACK
#define BB_WIFI_GW_PROBE_STACK 3072
#endif

#ifdef CONFIG_BB_WIFI_GW_PROBE_PRIORITY
#define BB_WIFI_GW_PROBE_PRIORITY CONFIG_BB_WIFI_GW_PROBE_PRIORITY
#endif
#ifndef BB_WIFI_GW_PROBE_PRIORITY
#define BB_WIFI_GW_PROBE_PRIORITY 3
#endif

// Recorded status, protected by s_status_mux (small dedicated critical
// section — mirrors s_ap_mux's pattern in bb_wifi.c).
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static bb_wifi_gw_status_t s_status = {0};
static bool s_started = false;

// Observe-owned policy state — a SEPARATE instance from the live FSM's
// wifi_reconn_state_t (owned by wifi_reconn.c / s_state there). Deliberately
// never shared: feeding this worker's result into the live state would let
// an observe-only probe influence real recovery decisions, exactly what
// #578 removed.
static wifi_reconn_state_t s_observe_state;

static int64_t gw_probe_now_us(void)
{
    return (int64_t)bb_timer_now_us();
}

static const wifi_reconn_adapter_t s_observe_adapter = {
    .now_us = gw_probe_now_us,
};

static void gw_probe_work_fn(void *arg)
{
    (void)arg;

    // Only probe when associated with an IP — a gateway ping with no route
    // is meaningless (bb_wifi_gateway_reachable would just return false).
    if (!bb_wifi_is_associated() || !bb_wifi_has_ip()) {
        return;
    }

    bool reachable = bb_wifi_gateway_reachable(BB_WIFI_GW_PROBE_TIMEOUT_MS);
    uint64_t now = bb_clock_now_ms64();

    // Reuse the pure classifier against the SEPARATE observe state. The
    // returned action is captured only to COUNT a would-have-tripped event
    // (gw_dead_count) — it is intentionally DISCARDED otherwise: no
    // bb_wifi_restart_sta / bb_wifi_request_recovery / esp_wifi_* call is
    // ever made from this worker. Counting the action must not become
    // acting on it — this discard is the observe/act boundary.
    wifi_reconn_action_t act =
        wifi_reconn_policy_on_egress_probe(&s_observe_state, &s_observe_adapter,
                                            reachable, BB_WIFI_GW_PROBE_FAILS);

    portENTER_CRITICAL(&s_status_mux);
    s_status.gw_reachable = reachable;
    s_status.gw_fail_streak = s_observe_state.egress_fail_streak;
    s_status.gw_probe_count++;
    if (!reachable) {
        s_status.gw_fail_count++;
    }
    if (act == WIFI_RECONN_ACTION_RECONNECT_NOW) {
        s_status.gw_dead_count++;
    }
    s_status.last_gw_probe_ms = now;
    portEXIT_CRITICAL(&s_status_mux);
}

bb_err_t bb_wifi_get_gateway_status(bb_wifi_gw_status_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (!s_started) return BB_ERR_INVALID_STATE;

    portENTER_CRITICAL(&s_status_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_mux);
    return BB_OK;
}

#if CONFIG_BB_WIFI_GW_PROBE_ENABLE
static bb_periodic_timer_t s_timer = NULL;

bb_err_t bb_wifi_gw_probe_start(void)
{
    wifi_reconn_state_reset(&s_observe_state);

    bb_timer_worker_cfg_t cfg = {
        .stack    = BB_WIFI_GW_PROBE_STACK,
        .priority = BB_WIFI_GW_PROBE_PRIORITY,
        .core     = -1,
    };
    bb_err_t err = bb_timer_worker_periodic_create(gw_probe_work_fn, NULL,
                                                    "bb_wifi_gwprobe", &cfg, &s_timer);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to create gw-probe worker timer: %d", (int)err);
        return err;
    }

    err = bb_timer_periodic_start(s_timer, (uint64_t)BB_WIFI_GW_PROBE_PERIOD_S * 1000000ULL);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to start gw-probe periodic timer: %d", (int)err);
        return err;
    }

    s_started = true;
    bb_log_i(TAG, "started (observe-only); period=%ds timeout=%dms fails=%d",
             BB_WIFI_GW_PROBE_PERIOD_S, BB_WIFI_GW_PROBE_TIMEOUT_MS, BB_WIFI_GW_PROBE_FAILS);
    return BB_OK;
}

#endif /* CONFIG_BB_WIFI_GW_PROBE_ENABLE */
