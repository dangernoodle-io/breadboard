// ESP-IDF route handler + worker task for bb_update_check. Timer triggers a
// FreeRTOS worker; worker calls bb_update_check_run_one(). The HTTP handler
// just reads the cached status.
#include "bb_update_check.h"
#include "bb_update_check_internal.h"
#include "bb_http.h"
#include "bb_http_client.h"
#include "bb_log.h"
#include "bb_registry.h"
#include "bb_timer.h"
#include "bb_json.h"
#include "bb_event_routes.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bb_update_check";

static bb_timer_handle_t s_timer = NULL;
static SemaphoreHandle_t s_kick  = NULL;
static TaskHandle_t      s_worker = NULL;

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
}

// ---------------------------------------------------------------------------
// GET /api/update/status
// ---------------------------------------------------------------------------

static bb_err_t status_handler(bb_http_request_t *req)
{
    bb_update_check_status_t st;
    bb_err_t err = bb_update_check_get_status(&st);
    if (err != BB_OK) {
        bb_http_resp_send_err(req, 503, "not initialized");
        return BB_OK;
    }

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_string(root, "current", st.current);
    bb_json_obj_set_string(root, "latest", st.latest);
    bb_json_obj_set_string(root, "download_url", st.download_url);
    bb_json_obj_set_bool(root, "available", st.available);
    bb_json_obj_set_bool(root, "last_check_ok", st.last_check_ok);
    // Unix seconds when last_check_us is non-zero; omitted otherwise so the
    // client can render "never checked" cleanly.
    if (st.last_check_us != 0) {
        bb_json_obj_set_number(root, "last_check_ts", (double)(st.last_check_us / 1000000));
    }
    bb_err_t r = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return r;
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
         "\"last_check_ts\":{\"type\":\"integer\"}},"
       "\"required\":[\"current\",\"latest\",\"download_url\","
                     "\"available\",\"last_check_ok\"]}",
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

    s_kick = xSemaphoreCreateBinary();
    if (!s_kick) return BB_ERR_NO_SPACE;
    // Stack sized for the mbedTLS handshake + cert-bundle parse path inside
    // bb_http_client_get_stream. Shared with bb_ota_pull via the same macro.
    if (xTaskCreate(worker_task, "upd_check", BB_HTTP_CLIENT_TASK_STACK,
                    NULL, 1, &s_worker) != pdPASS) {
        vSemaphoreDelete(s_kick);
        s_kick = NULL;
        return BB_ERR_INVALID_STATE;
    }

    // Create the periodic timer. interval comes from Kconfig; the worker is
    // a no-op until set_releases_url is called.
    uint64_t period_us = (uint64_t)CONFIG_BB_UPDATE_CHECK_INTERVAL_S * 1000000ULL;
    err = bb_timer_create("upd_check", BB_TIMER_PERIODIC, period_us, timer_cb, NULL, &s_timer);
    if (err != BB_OK) return err;
    bb_timer_start(s_timer);

#if defined(CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH) && CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH
    {
        // retained=true: update.available is a state topic — new SSE clients should
        // always receive the last known value even before the first periodic check fires.
        bb_err_t attach_err = bb_event_routes_attach_ex("update.available", true);
        if (attach_err != BB_OK) {
            bb_log_w(TAG, "auto-attach failed for 'update.available': %d", attach_err);
        }
    }
#endif

    bb_log_i(TAG, "registered /api/update/status; period=%" PRIu32 " s",
             (uint32_t)CONFIG_BB_UPDATE_CHECK_INTERVAL_S);
    return BB_OK;
}

#if CONFIG_BB_UPDATE_CHECK_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_update_check, bb_update_check_register_init, 4);
#endif
