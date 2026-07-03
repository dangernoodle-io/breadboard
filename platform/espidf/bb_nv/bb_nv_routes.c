#include "bb_nv.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_init.h"
#include "bb_clock.h"

#include <string.h>

#if CONFIG_BB_NV_FACTORY_RESET

#ifdef ESP_PLATFORM
#include "bb_timer.h"
#include "esp_system.h"

static const char *TAG = "bb_nv_routes";

static void factory_reset_reboot_work_fn(void *arg)
{
    (void)arg;
    uint32_t uptime_s = (uint32_t)(bb_clock_now_ms64() / 1000ULL);
    /* epoch_s=0: bb_nv has no bb_ntp dependency (would create an unwanted
     * edge); the boot-side reader treats epoch_s=0 as "unknown/unsynced"
     * per the record contract. */
    bb_err_t rc = bb_nv_reboot_record_save(BB_RESET_SRC_FACTORY_RESET, NULL, 0, uptime_s);
    if (rc == BB_ERR_INVALID_ARG) {
        bb_log_w(TAG, "factory_reset: record encode failed, rebooting without reason");
    } else if (rc != BB_OK) {
        bb_log_w(TAG, "factory_reset: NVS persist failed: %d", (int)rc);
    }
    esp_restart();
}
#endif /* ESP_PLATFORM */

#ifdef BB_NV_FACTORY_RESET_TESTING
/* Expose for host unit tests. */
bb_err_t bb_nv_factory_reset_handler_for_test(bb_http_request_t *req);
#endif

static bb_err_t factory_reset_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > 128) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "missing or oversized body");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    char body[128];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "read failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    if (!parsed) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "invalid JSON");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    char confirm[32];
    confirm[0] = '\0';
    bb_json_obj_get_string(parsed, "confirm", confirm, sizeof(confirm));
    bb_json_free(parsed);

    if (strcmp(confirm, "factory-reset") != 0) {
        bb_http_resp_set_status(req, 400);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error",
            "confirm field must be \"factory-reset\"");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t err = bb_nv_config_factory_reset();
    if (err != BB_OK) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "factory reset failed");
        bb_http_resp_json_obj_end(&obj);
        return err;
    }

    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "factory_reset_accepted");
    bb_http_resp_json_obj_set_bool(&obj, "reboot", true);
    bb_http_resp_json_obj_end(&obj);

#ifdef ESP_PLATFORM
    static bb_oneshot_timer_t s_reset_timer = NULL;
    if (!s_reset_timer) {
        bb_timer_deferred_oneshot_create(factory_reset_reboot_work_fn, NULL,
                                         "bb_nv_factory_reset", &s_reset_timer);
    }
    bb_timer_oneshot_stop(s_reset_timer);
    bb_timer_oneshot_start(s_reset_timer, 500 * 1000); /* 500 ms — lets HTTP 202 flush */
#endif /* ESP_PLATFORM */

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static const bb_route_response_t s_factory_reset_responses[] = {
    { 202, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"status\":{\"type\":\"string\"},"
      "\"reboot\":{\"type\":\"boolean\"}},"
      "\"required\":[\"status\",\"reboot\"]}",
      "factory reset accepted; device will reboot after ~500 ms" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "missing or invalid confirmation body" },
    { 0 },
};

static const bb_route_t s_factory_reset_route = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/factory-reset",
    .tag                  = "system",
    .summary              = "Erase all NVS config and reboot to factory defaults",
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\","
                            "\"properties\":{\"confirm\":{\"type\":\"string\"}},"
                            "\"required\":[\"confirm\"]}",
    .responses            = s_factory_reset_responses,
    .handler              = factory_reset_handler,
};

static bb_err_t bb_nv_factory_reset_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    return bb_http_register_described_route(server, &s_factory_reset_route);
}

#if CONFIG_BB_NV_FACTORY_RESET_AUTOREGISTER
BB_INIT_REGISTER(bb_nv_factory_reset_routes, bb_nv_factory_reset_routes_init);
#endif

#ifdef BB_NV_FACTORY_RESET_TESTING
bb_err_t bb_nv_factory_reset_handler_for_test(bb_http_request_t *req)
{
    return factory_reset_handler(req);
}
#endif

#endif /* CONFIG_BB_NV_FACTORY_RESET */
