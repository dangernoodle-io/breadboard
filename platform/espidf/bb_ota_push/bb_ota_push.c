#include "bb_ota_push.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "bb_http.h"
#include "bb_log.h"
#include "bb_registry.h"
#include "bb_system.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#endif

// Pluggable pause/resume callbacks
static bb_ota_pause_cb_t s_pause_cb = NULL;
static bb_ota_resume_cb_t s_resume_cb = NULL;

// Pluggable skip-check callback
static bb_ota_push_skip_check_cb_t s_skip_check_cb = NULL;

// Optional progress callback (LED/feedback) — shared bb_core typedef.
static bb_ota_progress_cb_t s_progress_cb = NULL;

// Public API: Set pause/resume callbacks
void bb_ota_push_set_hooks(bb_ota_pause_cb_t pause, bb_ota_resume_cb_t resume)
{
    s_pause_cb = pause;
    s_resume_cb = resume;
}

// Public API: Set progress callback (LED/feedback during the push)
void bb_ota_push_set_progress_cb(bb_ota_progress_cb_t cb)
{
    s_progress_cb = cb;
}

// Public API: Set skip-check callback
void bb_ota_push_set_skip_check_cb(bb_ota_push_skip_check_cb_t cb)
{
    s_skip_check_cb = cb;
}

/**
 * Validate the OTA push Content-Length.
 * Returns 0 if valid, 400 if content_len <= 0, 413 if content_len > max_size.
 * Exposed outside #ifdef ESP_PLATFORM for host unit testing.
 */
int bb_ota_push_validate_content_len(int content_len, int max_size)
{
    if (content_len <= 0) return 400;
    if (content_len > max_size) return 413;
    return 0;
}

#ifdef BB_OTA_PUSH_TESTING
int bb_ota_push_validate_content_len_for_test(int content_len, int max_size)
{
    return bb_ota_push_validate_content_len(content_len, max_size);
}
#endif

#ifdef ESP_PLATFORM

// bb_http_req_recv returns this value on socket timeout (mirrors httpd internal)
#define BB_OTA_RECV_TIMEOUT (-3)

static const char *TAG = "bb_ota_push";

#define OTA_RECV_BUF_SIZE CONFIG_BB_OTA_PUSH_RECV_BUF_SIZE
#define OTA_TIMEOUT_RETRIES 30

// Fire the optional progress callback (LED/feedback). No-op if unset.
static void ota_push_progress(bb_ota_phase_t phase, int pct)
{
    bb_ota_progress_cb_t cb = s_progress_cb;
    if (cb) cb(phase, pct);
}

/**
 * POST /api/update/push - Receive and flash firmware via HTTP upload.
 * Expects raw binary .bin file in request body.
 */
static bb_err_t ota_push_handler(bb_http_request_t *req)
{
    int content_len = bb_http_req_body_len(req);
    int validate_status = bb_ota_push_validate_content_len(
        content_len, CONFIG_BB_OTA_PUSH_MAX_SIZE);
    if (validate_status != 0) {
        bb_log_e(TAG, "OTA push rejected: content_len=%d, status=%d",
                 content_len, validate_status);
        if (validate_status == 413) {
            bb_http_resp_set_status(req, 413);
        } else {
            bb_http_resp_set_status(req, 400);
        }
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error",
            validate_status == 413 ? "Payload Too Large" : "Invalid Content-Length");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_ARG;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "No OTA partition");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        bb_log_e(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "OTA begin failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }

    bb_log_i(TAG, "OTA started, receiving to partition '%s' at 0x%"PRIx32,
             partition->label, partition->address);

    // Pause work and extend WDT for the receive+write phase. esp_ota_write
    // cache_disable windows + the consumer pause window can exceed
    // CONFIG_ESP_TASK_WDT_TIMEOUT_S; subscribed tasks (idle, mining) would
    // otherwise trip. Restored on every exit path (success path reboots).
    bool s_paused = (s_pause_cb != NULL) ? s_pause_cb() : false;
    bb_system_wdt_set_timeout(CONFIG_BB_OTA_PUSH_WDT_EXTENDED_S);  // push proceeds regardless of pause result

    char *buf = malloc(OTA_RECV_BUF_SIZE);
    if (!buf) {
        bb_log_e(TAG, "malloc failed for OTA receive buffer");
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "malloc failed");
        bb_http_resp_json_obj_end(&obj);
        bb_system_wdt_set_timeout(CONFIG_ESP_TASK_WDT_TIMEOUT_S);
        if (s_paused && s_resume_cb) { s_resume_cb(); }
        return BB_ERR_NO_SPACE;
    }

    int received = 0;
    int timeout_count = 0;
    int last_pct = -1;
    ota_push_progress(BB_OTA_PHASE_START, 0);

    while (received < content_len) {
        int ret = bb_http_req_recv(req, buf,
            OTA_RECV_BUF_SIZE < (content_len - received) ? OTA_RECV_BUF_SIZE : (content_len - received));

        if (ret == BB_OTA_RECV_TIMEOUT) {
            if (++timeout_count > OTA_TIMEOUT_RETRIES) {
                bb_log_e(TAG, "OTA upload timeout after %d retries", timeout_count);
                esp_ota_abort(ota_handle);
                bb_http_resp_set_status(req, 408);
                bb_http_json_obj_stream_t obj;
                bb_http_resp_json_obj_begin(req, &obj);
                bb_http_resp_json_obj_set_str(&obj, "error", "Upload timeout");
                bb_http_resp_json_obj_end(&obj);
                free(buf);
                goto resume_and_exit;
            }
            continue;
        }

        timeout_count = 0;
        if (ret <= 0) {
            bb_log_e(TAG, "OTA receive error at %d/%d", received, content_len);
            esp_ota_abort(ota_handle);
            bb_http_resp_set_status(req, 500);
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "error", "Receive failed");
            bb_http_resp_json_obj_end(&obj);
            free(buf);
            goto resume_and_exit;
        }

        // Board-name validation on first chunk
        if (received == 0 && ret >= (int)(sizeof(esp_image_header_t) +
            sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))) {
            const esp_app_desc_t *incoming = (const esp_app_desc_t *)
                (buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
            const esp_app_desc_t *running = esp_app_get_description();

            if (strncmp(incoming->project_name, running->project_name,
                        sizeof(incoming->project_name)) != 0) {
                bool skip_check = (s_skip_check_cb != NULL && s_skip_check_cb());
                if (skip_check) {
                    bb_log_w(TAG, "OTA board mismatch IGNORED (skip_check): "
                             "firmware is for '%s', this device is '%s'",
                             incoming->project_name, running->project_name);
                } else {
                    bb_log_e(TAG, "OTA rejected: firmware is for '%s', this device is '%s'",
                             incoming->project_name, running->project_name);
                    esp_ota_abort(ota_handle);
                    bb_http_resp_set_status(req, 400);
                    bb_http_json_obj_stream_t bm_obj;
                    bb_http_resp_json_obj_begin(req, &bm_obj);
                    bb_http_resp_json_obj_set_str(&bm_obj, "error", "Firmware board mismatch");
                    bb_http_resp_json_obj_end(&bm_obj);
                    free(buf);
                    goto resume_and_exit;
                }
            }
            bb_log_i(TAG, "OTA board check passed: %s", incoming->project_name);
        }

        err = esp_ota_write(ota_handle, buf, ret);
        if (err != ESP_OK) {
            bb_log_e(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            bb_http_resp_set_status(req, 500);
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "error", "OTA write failed");
            bb_http_resp_json_obj_end(&obj);
            free(buf);
            goto resume_and_exit;
        }

        received += ret;
        if (content_len > 0) {
            int pct = (int)((int64_t)received * 100 / content_len);
            if (pct / 10 != last_pct / 10) {
                ota_push_progress(BB_OTA_PHASE_PROGRESS, pct);
                last_pct = pct;
            }
        }

        // Yield each iteration so the IDLE task (and the WiFi/lwIP stack) get
        // CPU during the receive+write loop. On single-core targets the httpd
        // worker otherwise monopolizes the core for the whole transfer and the
        // task WDT trips the idle task — the extended WDT timeout only delays
        // that, it does not prevent it. One tick per ~4 KB chunk is negligible
        // overhead (a few seconds across a 1 MB image) and also lets the TCP
        // window refill, improving throughput.
        vTaskDelay(1);
    }

    bb_log_i(TAG, "OTA receive complete (%d bytes), validating", received);
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        bb_log_e(TAG, "esp_ota_end failed: %s (0x%x)", esp_err_to_name(err), err);
        goto resume_and_exit;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        bb_log_e(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        goto resume_and_exit;
    }

    bb_log_i(TAG, "OTA complete, rebooting");
    ota_push_progress(BB_OTA_PHASE_SUCCESS, 100);
    {
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "status", "rebooting");
        bb_http_resp_json_obj_end(&obj);
    }

    // Do not resume the consumer's worker (e.g. mining task) on the
    // success path — we are about to esp_restart(). Resuming here lets
    // the worker spin up for ~500ms only to be killed mid-work, wasting
    // CPU/heat and competing with the reboot path for resources. The
    // failure path (resume_and_exit below) still resumes correctly.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return BB_OK;  // unreachable

resume_and_exit:
    ota_push_progress(BB_OTA_PHASE_FAIL, 0);
    bb_system_wdt_set_timeout(CONFIG_ESP_TASK_WDT_TIMEOUT_S);
    if (s_paused && s_resume_cb) { s_resume_cb(); }
    return BB_ERR_INVALID_STATE;
}

// ---------------------------------------------------------------------------
// Route descriptor (handler registered via raw httpd API)
// ---------------------------------------------------------------------------

static const bb_route_response_t s_ota_push_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "OTA complete; device rebooting" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "firmware board mismatch, invalid binary, or Content-Length <= 0" },
    { 408, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "upload timeout" },
    { 413, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "payload exceeds CONFIG_BB_OTA_PUSH_MAX_SIZE (default 4 MB)" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "OTA write or validation failed, allocation failure, or no OTA partition" },
    { 0 },
};

static const bb_route_t s_ota_push_route = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/update/push",
    .tag                  = "update",
    .summary              = "Upload and flash firmware binary",
    .request_content_type = "application/octet-stream",
    .request_schema       = NULL,  // raw binary .bin file
    .responses            = s_ota_push_responses,
    .handler              = NULL,
};

/**
 * Register OTA push HTTP handler with an existing httpd instance.
 */
static bb_err_t bb_ota_push_init(bb_http_handle_t server)
{
    if (!server) {
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t err = bb_http_register_route(server, BB_HTTP_POST,
                                          "/api/update/push", ota_push_handler);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register /api/update/push handler");
        return err;
    }

    // Add descriptor for OpenAPI spec emission.
    bb_err_t desc_err = bb_http_register_route_descriptor_only(&s_ota_push_route);
    if (desc_err != BB_OK) {
        bb_log_e(TAG, "failed to register ota-push descriptor: %d", desc_err);
    }

    bb_log_i(TAG, "OTA push handler registered");
    return BB_OK;
}

#if CONFIG_BB_OTA_PUSH_AUTOREGISTER
static bb_err_t bb_ota_push_reserve_routes(void)
{
    bb_http_reserve_routes(1);  // POST /api/update/push
    return BB_OK;
}
BB_REGISTRY_REGISTER_PRE_HTTP(bb_ota_push, bb_ota_push_reserve_routes);
BB_REGISTRY_REGISTER_N(bb_ota_push, bb_ota_push_init, 1);
#endif

#endif // ESP_PLATFORM
