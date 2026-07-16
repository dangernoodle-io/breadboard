#include "wifi_reconn.h"
#include "wifi_reconn_policy.h"
#include "bb_fsm.h"
#include "bb_timer.h"
#include "bb_wifi.h"
#include "bb_task.h"
#include "bb_system.h"

#include <inttypes.h>

#include "esp_wifi.h"
#include "bb_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "wifi_reconn";

// B1-805 slice 1a: the ST_IDLE zombie-watchdog poll that consumed this flag
// lived in the now-deleted switch-based reconn_task. The bb_fsm rebuild has
// no equivalent state-shaped poll yet (deferred to slice 1b) -- fail the
// build loudly rather than silently shipping a build where the Kconfig knob
// is on but nothing implements it.
#if BB_WIFI_NO_IP_WATCHDOG_ENABLE
#error "no_ip_watchdog recovery not yet integrated with the bb_fsm reconn -- see B1-805 slice 1b"
#endif

// Mirrors the tripwire above (review fix [MEDIUM], B1-805 slice 1a): the
// restart-based recovery half of BB_WIFI_INACTIVE_TIME_ENABLE (the old
// switch-based reconn_task's ST_IDLE watchdog calling bb_wifi_restart_sta()
// on beacon-loss) was deleted along with it -- the adapter's restart_sta_fn
// is gone too (see wifi_reconn_policy.h). What remains
// (esp_wifi_set_inactive_time in bb_wifi.c) still makes the driver emit a
// normal WIFI_EVENT_STA_DISCONNECTED, which the FSM does handle -- but fail
// the build loudly rather than let this knob quietly rely on that as an
// unreviewed accident of the new design.
#if BB_WIFI_INACTIVE_TIME_ENABLE
#error "inactive-time restart-reconnect not yet integrated with the bb_fsm reconn -- see B1-805 slice 1b"
#endif

#define RECONN_QUEUE_LEN 8
// 6144 (not 4096): the escalate-reboot path opens NVS + writes the boot-fail
// count (nvs_open/set/commit) via bb_system_boot_count_increment/
// bb_system_reboot_budget_record, which can consume ~1.5-2 KB of stack on
// top of the reconn task's normal usage -- 4096 left too little headroom
// (B1-330).
#define RECONN_TASK_STACK 6144
#define RECONN_TASK_PRIO 6
#define RECONN_TASK_CORE 0

typedef struct {
    bb_fsm_event_t fsm_event;
    uint8_t         reason; // meaningful only for EV_STA_DISCONNECTED
} reconn_evt_t;

// Task + queue handles
static TaskHandle_t  s_task  = NULL;
static QueueHandle_t s_queue = NULL;
static volatile bool s_active = false;

// FSM context -- embedded by value, zero heap, single-writer (the reconn
// task is the sole bb_fsm_step()/arm/disarm caller). s_ctx.self_disconnect
// is read/cleared by the disconnect NOTIFIER below (event-handler task
// context, NOT inside a guard/action/hook) and set from the reconn task
// (act_timeout_reattempt) or an arbitrary caller task
// (wifi_reconn_absorb_next_disconnect(), e.g. bb_wifi_restart_sta()) --
// three distinct FreeRTOS tasks can touch it. esp_wifi_disconnect() does
// NOT emit WIFI_EVENT_STA_DISCONNECTED synchronously before returning; it's
// dispatched via the default event-loop task, so there is no run-to-
// completion ordering to rely on here. self_disconnect is `volatile` (see
// wifi_reconn_ctx_t) precisely so this cross-task absorb guard is visible
// across all of them.
static wifi_reconn_ctx_t s_ctx;

// Lost-IP diagnostic state (review fix [MEDIUM], B1-805 slice 1a): owned
// solely by the WiFi event-loop task (the only caller of
// wifi_reconn_on_lost_ip, via IP_EVENT_STA_LOST_IP) -- a DIFFERENT task than
// the reconn task that owns s_ctx. Deliberately NOT s_ctx.policy: writing
// into the FSM's single-writer state from this task would recreate the same
// cross-task-write bug class as self_disconnect above. Reuses the existing,
// already-100%-host-tested wifi_reconn_policy_on_lost_ip pure function
// (wifi_reconn_policy.c) against this dedicated instance -- only its
// lost_ip_count/last_lost_ip_us fields (and the LOST_IP histogram slot) are
// meaningful here; the rest of wifi_reconn_state_t goes unused by this
// instance.
static wifi_reconn_state_t s_lost_ip_diag;

// --- Adapter (R3): every side-effecting call an FSM action/hook makes goes
// through one of these. Real wiring -- the ONE file-static instance below.
static int64_t reconn_now_us(void) { return (int64_t)bb_timer_now_us(); }
static void    reconn_connect(void) { esp_wifi_connect(); }
static void    reconn_disconnect(void) { esp_wifi_disconnect(); }
static bool    reconn_budget_allows(void) { return bb_system_reboot_budget_allows(BB_REBOOT_CAUSE_WIFI_SAFEGUARD); }
static void    reconn_budget_record(void) { bb_system_reboot_budget_record(BB_REBOOT_CAUSE_WIFI_SAFEGUARD); }
static bool    reconn_boot_fail_over(void) { return bb_system_boot_fail_over_threshold(); }
static void    reconn_boot_count_increment(void)
{
    (void)bb_system_boot_count_increment();
    bb_log_w(TAG, "wifi safeguard reboot: fail_count=%u threshold=%u",
              (unsigned)bb_system_boot_count_get(), (unsigned)BB_SYSTEM_BOOT_FAIL_THRESHOLD);
}
static bool    reconn_ota_validated(void) { return bb_wifi_internal_ota_validated(); }
static void    reconn_reboot(const char *detail) { bb_system_restart_reason(BB_RESET_SRC_WIFI_SAFEGUARD, detail); }
static void    reconn_emit_net_event(bb_wifi_net_event_t evt, bb_wifi_disc_reason_t reason)
{
    // Bench/production visibility: escalation-denied was previously only
    // observable indirectly (via the mDNS restart it also triggers) -- log
    // it explicitly rather than adding a log for every net event.
    if (evt == BB_WIFI_NET_EVT_REBOOT_DENIED) {
        bb_log_w(TAG, "wifi safeguard reboot denied by throttle (fail_count=%u threshold=%u)",
                  (unsigned)bb_system_boot_count_get(), (unsigned)BB_SYSTEM_BOOT_FAIL_THRESHOLD);
    }
    bb_wifi_publish_net_event(evt, reason);
}

static const wifi_reconn_adapter_t s_adapter = {
    .now_us                = reconn_now_us,
    .connect_fn             = reconn_connect,
    .disconnect_fn          = reconn_disconnect,
    .budget_allows_fn       = reconn_budget_allows,
    .budget_record_fn       = reconn_budget_record,
    .boot_fail_over_fn      = reconn_boot_fail_over,
    .boot_count_increment_fn = reconn_boot_count_increment,
    .ota_validated_fn       = reconn_ota_validated,
    .reboot_fn              = reconn_reboot,
    .emit_net_event_fn      = reconn_emit_net_event,
};

static void reconn_task(void *arg)
{
    (void)arg;
    bb_log_i(TAG, "reconnect manager started");

    for (;;) {
        TickType_t wait = portMAX_DELAY;
        bb_fsm_event_t tev;
        uint32_t tms;
        if (bb_fsm_timer_at(&s_ctx.fsm, 0, &tev, &tms)) {
            wait = pdMS_TO_TICKS(tms);
        }

        reconn_evt_t evt;
        if (xQueueReceive(s_queue, &evt, wait) == pdTRUE) {
            bb_fsm_step(&s_ctx.fsm, evt.fsm_event, &evt.reason);
        } else if (bb_fsm_timer_at(&s_ctx.fsm, 0, &tev, NULL)) {
            // Timer expiry: no event arrived within the armed window.
            bb_fsm_step(&s_ctx.fsm, tev, NULL);
        }
    }
}

void wifi_reconn_start(bool has_creds)
{
    if (s_active) return;

    s_queue = xQueueCreate(RECONN_QUEUE_LEN, sizeof(reconn_evt_t));
    if (!s_queue) {
        bb_log_e(TAG, "failed to create reconn queue");
        return;
    }

    s_ctx.adapter = &s_adapter;
    if (wifi_reconn_fsm_init(&s_ctx, has_creds ? WR_CONNECTING : WR_NO_CREDS) != BB_OK) {
        bb_log_e(TAG, "failed to init reconnect FSM");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }

    bb_task_config_t reconn_cfg = {
        .entry       = reconn_task,
        .name        = "wifi_reconn",
        .arg         = NULL,
        .stack_bytes = RECONN_TASK_STACK,
        .priority    = RECONN_TASK_PRIO,
        .core        = RECONN_TASK_CORE,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm     = false,
    };
    if (bb_task_create(&reconn_cfg, (void **)&s_task) != BB_OK) {
        bb_log_e(TAG, "failed to create reconn task");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    s_active = true;
}

bool wifi_reconn_is_active(void)
{
    return s_active;
}

void wifi_reconn_on_disconnect(uint8_t reason)
{
    if (!s_queue) return;
    // Absorb disconnects that we triggered ourselves (connect-timeout re-attempt,
    // act_timeout_reattempt in wifi_reconn_policy.c). esp_wifi_disconnect() does
    // NOT emit WIFI_EVENT_STA_DISCONNECTED synchronously before returning -- it's
    // dispatched asynchronously via the default event-loop task -- so by the
    // time this handler (itself running on that event-loop task) observes the
    // event, s_ctx.self_disconnect may have been set by the reconn task at any
    // earlier point; volatile is what makes that write visible here.
    // Clearing it here prevents a double fail-count bump and a spurious
    // second esp_wifi_connect() from re-entering the FSM.
    if (s_ctx.self_disconnect) {
        s_ctx.self_disconnect = false;
        bb_log_d(TAG, "absorbing self-induced disconnect (connect-timeout re-attempt)");
        return;
    }
    reconn_evt_t evt = { .fsm_event = EV_STA_DISCONNECTED, .reason = reason };
    if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        bb_log_w(TAG, "queue full, dropping disconnect event");
    }
}

void wifi_reconn_on_got_ip(void)
{
    if (!s_queue) return;
    reconn_evt_t evt = { .fsm_event = EV_GOT_IP, .reason = 0 };
    if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        bb_log_w(TAG, "queue full, dropping got-ip event");
    }
}

void wifi_reconn_on_lost_ip(void)
{
    // LOG-ONLY (R11): IP_EVENT_STA_LOST_IP is a 120s-debounced, Espressif-
    // documented "generally ignore" debug event, not a disconnect signal.
    // WIFI_EVENT_STA_DISCONNECTED (wifi_reconn_on_disconnect) is the sole
    // authoritative disconnect trigger -- no FSM mutation, no forced
    // reconnect here. Diagnostics ARE still recorded (review fix [MEDIUM]),
    // into s_lost_ip_diag -- a dedicated, event-task-owned instance, NOT
    // s_ctx.policy (see the s_lost_ip_diag comment above).
    wifi_reconn_policy_on_lost_ip(&s_lost_ip_diag, &s_adapter);
    bb_log_w(TAG, "IP lost while associated (log-only, not a disconnect trigger; lost_ip_count=%" PRIu32 ")",
             s_lost_ip_diag.lost_ip_count);
}

uint32_t wifi_reconn_get_lost_ip_count(void)
{
    return s_lost_ip_diag.lost_ip_count;
}

int64_t wifi_reconn_get_lost_ip_age_us(void)
{
    if (s_lost_ip_diag.last_lost_ip_us == 0) return 0;
    return (int64_t)bb_timer_now_us() - s_lost_ip_diag.last_lost_ip_us;
}

uint32_t wifi_reconn_get_egress_dead_count(void)
{
    return s_ctx.policy.egress_dead_count;
}

uint32_t wifi_reconn_get_no_ip_count(void)
{
    return s_ctx.policy.no_ip_count;
}

void wifi_reconn_get_disconnect(bb_wifi_disc_reason_t *reason, int64_t *age_us)
{
    if (reason) *reason = s_ctx.policy.last_reason;
    if (age_us) {
        int64_t t = s_ctx.policy.last_disconnect_us;
        *age_us = (t == 0) ? 0 : ((int64_t)bb_timer_now_us() - t);
    }
}

int wifi_reconn_get_retry_count(void)
{
    return s_ctx.policy.retry_count;
}

void wifi_reconn_get_histogram(uint16_t *out, size_t len)
{
    if (!out) return;
    if (len > BB_WIFI_DISC_COUNT) len = BB_WIFI_DISC_COUNT;
    for (size_t i = 0; i < len; i++) {
        out[i] = s_ctx.policy.reason_histogram[i];
    }
}

void wifi_reconn_absorb_next_disconnect(void)
{
    s_ctx.self_disconnect = true;
}

void wifi_reconn_request_recovery(const char *reason)
{
    // B1-805 slice 1a (R4): a no-op under the bb_fsm reconn. The only
    // production caller (bb_net_health tier-2) is gated behind
    // CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE (default OFF); the #error below
    // fails the build the moment that gate is flipped on, so this can't
    // silently ship stale until the FSM integration lands in slice 1b.
#if CONFIG_BB_NET_HEALTH_EGRESS_ACT_ENABLE
#error "egress-recovery tier-2 request path not yet integrated with the bb_fsm reconn -- see B1-805 slice 1b"
#endif
    (void)reason;
}
