// ESP-IDF route handler + worker task for bb_update_check. Timer triggers a
// FreeRTOS worker; worker calls bb_update_check_run_one(). The HTTP handler
// just reads the cached status.
#include "bb_update_check.h"
#include "bb_update_check_internal.h"
#include "bb_http.h"
#include "bb_http_client.h"
#include "bb_log.h"
#include "bb_registry.h"
#include "bb_event_routes.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/time.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bb_update_check";

// Jitter range in seconds applied to each periodic reschedule.
#define BB_UPDATE_CHECK_JITTER_S  600   // ±10 minutes
#define BB_UPDATE_CHECK_FLOOR_S    60   // minimum interval

static esp_timer_handle_t s_timer = NULL;
static SemaphoreHandle_t  s_kick  = NULL;
static TaskHandle_t       s_worker = NULL;
// Default: Core 1. On dual-core boards Core 0 carries lwip + wifi + httpd +
// the consumer's stratum/control-plane tasks; the mbedTLS handshake runs
// CPU-bound for 200–600 ms per attempt and would starve IDLE0 past the task
// watchdog if it landed there. Mirrors bb_ota_pull's default (Core 1).
// Consumers can opt out via bb_update_check_set_task_core(tskNO_AFFINITY)
// or pin elsewhere. See bb_update_check.h.
static int                s_task_core = 1;
// Default worker priority is 1 (low). Consumers running the worker on a core
// that also hosts a high-priority CPU-bound task (e.g. mining at prio 20)
// must raise this above that task's priority via bb_update_check_set_task_priority;
// otherwise the kick semaphore wakes the worker but it never gets CPU to call
// the pause hook. See bb_update_check.h.
static int                s_task_priority = 1;

// Compute next poll interval: CONFIG_BB_UPDATE_CHECK_INTERVAL_S ± jitter,
// floored at BB_UPDATE_CHECK_FLOOR_S.  Uses esp_random() for uniform jitter.
static uint64_t next_interval_us(void)
{
    // esp_random() returns a full 32-bit uniform random value.
    // Map to [0, 2*JITTER_S] then subtract JITTER_S for signed offset.
    uint32_t r = esp_random();
    int32_t  offset_s = (int32_t)(r % (uint32_t)(2 * BB_UPDATE_CHECK_JITTER_S + 1))
                        - BB_UPDATE_CHECK_JITTER_S;
    int32_t  interval_s = (int32_t)CONFIG_BB_UPDATE_CHECK_INTERVAL_S + offset_s;
    if (interval_s < BB_UPDATE_CHECK_FLOOR_S) {
        interval_s = BB_UPDATE_CHECK_FLOOR_S;
    }
    return (uint64_t)interval_s * 1000000ULL;
}

static void worker_task(void *arg)
{
    (void)arg;
    while (1) {
        xSemaphoreTake(s_kick, portMAX_DELAY);
        bb_update_check_run_one();
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    if (s_kick) xSemaphoreGive(s_kick);
    // Reschedule one-shot timer with jitter so back-to-back polls from a
    // fleet that rebooted together drift apart over time.
    esp_timer_start_once(s_timer, next_interval_us());
}

// ---------------------------------------------------------------------------
// Non-blocking kick (public API)
// ---------------------------------------------------------------------------

bb_err_t bb_update_check_kick(void)
{
    if (!s_kick) return BB_ERR_INVALID_STATE;
    xSemaphoreGive(s_kick);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// GET /api/update/status
// ---------------------------------------------------------------------------

// Map outcome enum to the JSON string value for /api/update/status.
// These exact strings are part of the public API consumed by TaipanMiner webui
// and taipan-cli. Do not rename without a coordinated consumer update.
static const char *outcome_str(bb_update_check_outcome_t o)
{
    switch (o) {
        case BB_UPDATE_OUTCOME_UP_TO_DATE: return "up_to_date";
        case BB_UPDATE_OUTCOME_AVAILABLE:  return "available";
        case BB_UPDATE_OUTCOME_NO_ASSET:   return "no_asset";
        case BB_UPDATE_OUTCOME_FAILED:     return "check_failed";
        default:                           return "unknown";
    }
}

static bb_err_t status_handler(bb_http_request_t *req)
{
    bb_update_check_status_t st;
    bb_err_t err = bb_update_check_get_status(&st);
    if (err != BB_OK) {
        bb_http_resp_set_status(req, 503);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "not initialized");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bb_http_json_obj_stream_t obj;
    err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj,  "current",       st.current);
    bb_http_resp_json_obj_set_str(&obj,  "latest",        st.latest);
    bb_http_resp_json_obj_set_str(&obj,  "download_url",  st.download_url);
    bb_http_resp_json_obj_set_bool(&obj, "available",     st.available);
    bb_http_resp_json_obj_set_bool(&obj, "last_check_ok", st.last_check_ok);
    bb_http_resp_json_obj_set_bool(&obj, "enabled",       st.enabled);
    bb_http_resp_json_obj_set_str(&obj,  "outcome",       outcome_str(st.outcome));
    // Unix seconds when last_check_us is non-zero; omitted otherwise so the
    // client can render "never checked" cleanly.
    if (st.last_check_us != 0) {
        bb_http_resp_json_obj_set_int(&obj, "last_check_ts",
                                      (int64_t)(st.last_check_us / 1000000));
    }
    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_status_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
       "\"properties\":{"
         "\"current\":{\"type\":\"string\"},"
         "\"latest\":{\"type\":\"string\"},"
         "\"download_url\":{\"type\":\"string\"},"
         "\"available\":{\"type\":\"boolean\"},"
         "\"last_check_ok\":{\"type\":\"boolean\"},"
         "\"enabled\":{\"type\":\"boolean\"},"
         "\"outcome\":{\"type\":\"string\","
           "\"enum\":[\"unknown\",\"up_to_date\",\"available\","
                     "\"no_asset\",\"check_failed\"]},"
         "\"last_check_ts\":{\"type\":\"integer\"}},"
       "\"required\":[\"current\",\"latest\",\"download_url\","
                     "\"available\",\"last_check_ok\",\"enabled\",\"outcome\"]}",
      "current status of the update poller" },
    { 503, "application/json", NULL, "bb_update_check not initialized" },
    { 0 },
};

static const bb_route_t s_status_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/update/status",
    .tag      = "update",
    .summary  = "Latest known release-check state",
    .responses = s_status_responses,
    .handler  = status_handler,
};

static bb_err_t bb_update_check_register_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_update_check_init(NULL);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_status_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, bb_update_check_config_get_route());
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, bb_update_check_config_post_route());
    if (err != BB_OK) return err;

    s_kick = xSemaphoreCreateBinary();
    if (!s_kick) return BB_ERR_NO_SPACE;
    // Stack sized for the mbedTLS handshake + cert-bundle parse path inside
    // bb_http_client_get_stream. Shared with bb_ota_pull via the same macro.
    // On single-core (unicore) targets, core 1 does not exist and
    // xTaskCreatePinnedToCore asserts; fall back to no affinity.
    int task_core = s_task_core;
    if (task_core != tskNO_AFFINITY && task_core >= configNUMBER_OF_CORES) {
        task_core = tskNO_AFFINITY;
    }
    if (xTaskCreatePinnedToCore(worker_task, "upd_check", BB_HTTP_CLIENT_TASK_STACK,
                                NULL, s_task_priority, &s_worker, task_core) != pdPASS) {
        vSemaphoreDelete(s_kick);
        s_kick = NULL;
        return BB_ERR_INVALID_STATE;
    }

    // Create a one-shot timer; timer_cb reschedules it with ±10 min jitter
    // each tick so a fleet that rebooted together drifts apart over time.
    esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .arg      = NULL,
        .name     = "upd_check",
    };
    err = esp_timer_create(&timer_args, &s_timer);
    if (err != BB_OK) return err;
    esp_timer_start_once(s_timer, next_interval_us());

#if defined(CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH) && CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH
    {
        // retained=true: update.available is a state topic — new SSE clients should
        // always receive the last known value even before the first periodic check fires.
        bb_err_t attach_err = bb_event_routes_attach_ex(BB_UPDATE_CHECK_TOPIC, true);
        if (attach_err != BB_OK) {
            bb_log_w(TAG, "auto-attach failed for 'update.available': %d", attach_err);
        }

        // Now that the ring is attached, publish the initial snapshot so that SSE
        // clients connecting before the first periodic check (up to
        // CONFIG_BB_UPDATE_CHECK_INTERVAL_S seconds) replay this entry rather than
        // seeing empty state.
        err = bb_update_check_publish_initial();
        if (err != BB_OK) {
            bb_log_w(TAG, "failed to publish initial snapshot: %d", err);
        }
    }
#endif

    bb_log_i(TAG, "registered /api/update/status; period=%" PRIu32 " s",
             (uint32_t)CONFIG_BB_UPDATE_CHECK_INTERVAL_S);
    return BB_OK;
}

void bb_update_check_set_task_core(int core)
{
    s_task_core = core;
}

void bb_update_check_set_task_priority(int priority)
{
    s_task_priority = priority;
}

#if CONFIG_BB_UPDATE_CHECK_AUTOREGISTER
static bb_err_t bb_update_check_reserve_routes(void)
{
    bb_http_reserve_routes(3);  // GET /api/update/status + GET /api/update/config + POST /api/update/config
    return BB_OK;
}
BB_REGISTRY_REGISTER_PRE_HTTP(bb_update_check, bb_update_check_reserve_routes);
BB_REGISTRY_REGISTER_N(bb_update_check, bb_update_check_register_init, 4);
#endif
