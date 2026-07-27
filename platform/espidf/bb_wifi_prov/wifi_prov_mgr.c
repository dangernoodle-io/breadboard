#include "wifi_prov_mgr.h"
#include "wifi_prov_policy.h"
#include "bb_wifi_prov.h"
#include "bb_wifi_ap.h"
#include "bb_wifi.h"
#include "bb_task.h"
#include "bb_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "wifi_prov_mgr";

// Kconfig bridge (see wiki Conventions#audit-class-regressions). Each
// CONFIG_BB_WIFI_PROV_* symbol (components/bb_wifi_prov/Kconfig) is bridged
// to its local name here; the #ifndef fallback below only ever fires on a
// host/non-ESP_PLATFORM build where sdkconfig.h doesn't exist.
#ifdef ESP_PLATFORM
#  include "sdkconfig.h"
#  ifdef CONFIG_BB_WIFI_PROV_TASK_STACK
#    define BB_WIFI_PROV_TASK_STACK CONFIG_BB_WIFI_PROV_TASK_STACK
#  endif
#  ifdef CONFIG_BB_WIFI_PROV_TASK_PRIORITY
#    define BB_WIFI_PROV_TASK_PRIORITY CONFIG_BB_WIFI_PROV_TASK_PRIORITY
#  endif
#  ifdef CONFIG_BB_WIFI_PROV_QUEUE_DEPTH
#    define BB_WIFI_PROV_QUEUE_DEPTH CONFIG_BB_WIFI_PROV_QUEUE_DEPTH
#  endif
#  ifdef CONFIG_BB_WIFI_PROV_SETTLE_MS
#    define BB_WIFI_PROV_SETTLE_MS CONFIG_BB_WIFI_PROV_SETTLE_MS
#  endif
#endif

#ifndef BB_WIFI_PROV_TASK_STACK
#define BB_WIFI_PROV_TASK_STACK 6144
#endif
#ifndef BB_WIFI_PROV_TASK_PRIORITY
#define BB_WIFI_PROV_TASK_PRIORITY 5
#endif
#ifndef BB_WIFI_PROV_QUEUE_DEPTH
#define BB_WIFI_PROV_QUEUE_DEPTH 8
#endif
#ifndef BB_WIFI_PROV_SETTLE_MS
#define BB_WIFI_PROV_SETTLE_MS 2000
#endif

#define BB_WIFI_PROV_TASK_CORE 0

typedef struct {
    wp_event_t fsm_event;
} prov_mgr_evt_t;

// Task + queue handles.
static TaskHandle_t  s_task  = NULL;
static QueueHandle_t s_queue = NULL;

// FSM context -- embedded by value, zero heap, single-writer: ONLY
// prov_mgr_task (below) may call bb_fsm_step()/wifi_prov_fsm_drive_entry()
// on s_ctx.fsm (see the "single-writer contract" comment on struct bb_fsm,
// bb_fsm.h, and on wifi_prov_ctx_t, wifi_prov_policy.h). Every other site
// (wifi_prov_mgr_on_save(), and PR4's future entry-trigger callers) posts
// to s_queue instead.
static wifi_prov_ctx_t s_ctx;

// Closed over by mgr_prov_start (below) so wifi_prov_policy.h's
// prov_start_fn can stay zero-arg -- bb_http_asset_t/
// bb_wifi_prov_extra_routes_fn_t must not leak into that pure host-
// compilable header (see its adapter comment).
static const bb_http_asset_t      *s_assets      = NULL;
static size_t                      s_asset_count = 0;
static bb_wifi_prov_extra_routes_fn_t s_extra    = NULL;

// --- Adapter: every side-effecting call an FSM action/hook makes goes
// through one of these. Real wiring -- the ONE file-static instance below.
static bb_err_t mgr_ap_start(void) { return bb_wifi_ap_start(); }
static void     mgr_ap_stop(void) { bb_wifi_ap_stop(); }
static bb_err_t mgr_prov_start(void) { return bb_wifi_prov_start(s_assets, s_asset_count, s_extra); }
static void     mgr_prov_stop(void) { bb_wifi_prov_stop(); }
static void     mgr_creds_arrived(void) { bb_wifi_on_creds_arrived(); }

static void mgr_log(wifi_prov_log_evt_t evt)
{
    switch (evt) {
        case WIFI_PROV_LOG_ENTRY_FIRST_BOOT:
            bb_log_i(TAG, "provisioning entry: first boot, no creds");
            break;
        case WIFI_PROV_LOG_ENTRY_DEPROV_ANOMALY:
            bb_log_w(TAG, "provisioning entry: deprovisioning anomaly detected");
            break;
        case WIFI_PROV_LOG_ENTRY_USER_REQUESTED:
            bb_log_i(TAG, "provisioning entry: user requested");
            break;
        case WIFI_PROV_LOG_ENTRY_ALREADY_ACTIVE:
            bb_log_i(TAG, "provisioning entry: already active, ignoring");
            break;
        case WIFI_PROV_LOG_AP_START_FAILED:
            bb_log_e(TAG, "provisioning AP start failed");
            break;
        case WIFI_PROV_LOG_PROV_START_FAILED:
            bb_log_e(TAG, "provisioning HTTP routes start failed, AP rolled back");
            break;
        case WIFI_PROV_LOG_PORTAL_OPEN:
            bb_log_i(TAG, "provisioning portal open");
            break;
        case WIFI_PROV_LOG_PORTAL_CLOSED:
            bb_log_i(TAG, "provisioning portal closed");
            break;
    }
}

static const wifi_prov_adapter_t s_adapter = {
    .ap_start_fn             = mgr_ap_start,
    .ap_stop_fn               = mgr_ap_stop,
    .prov_start_fn            = mgr_prov_start,
    .prov_stop_fn              = mgr_prov_stop,
    .creds_arrived_notify_fn  = mgr_creds_arrived,
    .log_fn                    = mgr_log,
};

static void prov_mgr_task(void *arg)
{
    (void)arg;
    bb_log_i(TAG, "provisioning manager started");

    for (;;) {
        TickType_t wait = portMAX_DELAY;
        bb_fsm_event_t tev;
        uint32_t tms;
        if (bb_fsm_timer_at(&s_ctx.fsm, 0, &tev, &tms)) {
            wait = pdMS_TO_TICKS(tms);
        }

        prov_mgr_evt_t evt;
        if (xQueueReceive(s_queue, &evt, wait) == pdTRUE) {
            // DISPATCH SPLITS BY EVENT KIND: the three entry events are a
            // MULTI-HOP driver (wifi_prov_fsm_drive_entry -- see its
            // comment block in wifi_prov_policy.h for why); every other
            // event is a plain single-step FSM transition. Swapping this
            // would silently break the entry sequence (an entry event fed
            // straight to bb_fsm_step only ever runs hop 1).
            if (evt.fsm_event == EV_ENTRY_FIRST_BOOT ||
                evt.fsm_event == EV_ENTRY_DEPROV_ANOMALY ||
                evt.fsm_event == EV_ENTRY_USER_REQUESTED) {
                wifi_prov_fsm_drive_entry(&s_ctx, evt.fsm_event);
            } else {
                bb_fsm_step(&s_ctx.fsm, evt.fsm_event, NULL);
            }
        } else if (bb_fsm_timer_at(&s_ctx.fsm, 0, &tev, NULL)) {
            // Timer expiry: no event arrived within the armed window. The
            // only timer this FSM ever arms is EV_SETTLE_TIMEOUT
            // (wp_closing_on_entry) -- always a plain step, never a driver
            // hop.
            bb_fsm_step(&s_ctx.fsm, tev, NULL);
        }
    }
}

bb_err_t wifi_prov_mgr_init(const bb_http_asset_t *assets, size_t n,
                             bb_wifi_prov_extra_routes_fn_t extra)
{
    if (s_queue) return BB_OK; // already initialized -- idempotent

    s_assets      = assets;
    s_asset_count = n;
    s_extra       = extra;

    s_queue = xQueueCreate(BB_WIFI_PROV_QUEUE_DEPTH, sizeof(prov_mgr_evt_t));
    if (!s_queue) {
        bb_log_e(TAG, "failed to create provisioning manager queue");
        return BB_ERR_NO_MEM;
    }

    s_ctx.adapter = &s_adapter;
    if (wifi_prov_fsm_init(&s_ctx, WP_IDLE, BB_WIFI_PROV_SETTLE_MS) != BB_OK) {
        bb_log_e(TAG, "failed to init provisioning FSM");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return BB_ERR_INVALID_STATE;
    }

    bb_task_config_t cfg = {
        .entry       = prov_mgr_task,
        .name        = "wifi_prov_mgr",
        .arg         = NULL,
        .stack_bytes = BB_WIFI_PROV_TASK_STACK,
        .priority    = BB_WIFI_PROV_TASK_PRIORITY,
        .core        = BB_WIFI_PROV_TASK_CORE,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm     = false,
    };
    bb_err_t err = bb_task_create(&cfg, (void **)&s_task);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to create provisioning manager task");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return err;
    }
    return BB_OK;
}

void wifi_prov_mgr_on_save(void)
{
    if (!s_queue) return;
    prov_mgr_evt_t evt = { .fsm_event = EV_SAVE_SUCCESS };
    if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        bb_log_w(TAG, "queue full, dropping save-success event");
    }
}
