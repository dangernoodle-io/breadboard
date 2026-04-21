#include "bb_ota_push.h"
#include <string.h>

// Pluggable pause/resume callbacks
static bb_ota_pause_cb_t s_pause_cb = NULL;
static bb_ota_resume_cb_t s_resume_cb = NULL;

// Pluggable skip-check callback
static bb_ota_push_skip_check_cb_t s_skip_check_cb = NULL;

// Public API: Set pause/resume callbacks
void bb_ota_push_set_hooks(bb_ota_pause_cb_t pause, bb_ota_resume_cb_t resume)
{
    s_pause_cb = pause;
    s_resume_cb = resume;
}

// Public API: Set skip-check callback
void bb_ota_push_set_skip_check_cb(bb_ota_push_skip_check_cb_t cb)
{
    s_skip_check_cb = cb;
}

#ifdef ESP_PLATFORM
#include "bb_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

static const char *TAG = "bb_ota_push";

#define OTA_RECV_BUF_SIZE 1024
#define OTA_TIMEOUT_RETRIES 30

/**
 * POST /api/ota/push - Receive and flash firmware via HTTP upload.
 * Expects raw binary .bin file in request body.
 */
static esp_err_t ota_push_handler(httpd_req_t *req)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        bb_log_e(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    bb_log_i(TAG, "OTA started, receiving to partition '%s' at 0x%"PRIx32,
             partition->label, partition->address);

    // Pause work before beginning flash writes
    bool paused = false;
    if (s_pause_cb) {
        paused = s_pause_cb();
    }

    char buf[OTA_RECV_BUF_SIZE];
    int received = 0;
    int timeout_count = 0;
    int content_len = req->content_len;

    while (received < content_len) {
        int ret = httpd_req_recv(req, buf,
            sizeof(buf) < (content_len - received) ? sizeof(buf) : (content_len - received));

        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_count > OTA_TIMEOUT_RETRIES) {
                bb_log_e(TAG, "OTA upload timeout after %d retries", timeout_count);
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Upload timeout");
                goto resume_and_exit;
            }
            continue;
        }

        timeout_count = 0;
        if (ret <= 0) {
            bb_log_e(TAG, "OTA receive error at %d/%d", received, content_len);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
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
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware board mismatch");
                    goto resume_and_exit;
                }
            }
            bb_log_i(TAG, "OTA board check passed: %s", incoming->project_name);
        }

        err = esp_ota_write(ota_handle, buf, ret);
        if (err != ESP_OK) {
            bb_log_e(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            goto resume_and_exit;
        }

        received += ret;
    }

    bb_log_i(TAG, "OTA receive complete (%d bytes), validating", received);

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
    httpd_resp_sendstr(req, "OTA complete. Rebooting...");

    if (paused && s_resume_cb) {
        s_resume_cb();
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable

resume_and_exit:
    if (paused && s_resume_cb) {
        s_resume_cb();
    }
    return ESP_FAIL;
}

/**
 * Register OTA push HTTP handler with an existing httpd instance.
 */
esp_err_t bb_ota_push_register_handler(bb_http_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_handle_t s = (httpd_handle_t)server;

    httpd_uri_t push_uri = {
        .uri = "/api/ota/push",
        .method = HTTP_POST,
        .handler = ota_push_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(s, &push_uri);
    if (err != ESP_OK) {
        bb_log_e(TAG, "failed to register /api/ota/push handler: %s",
                 esp_err_to_name(err));
        return err;
    }

    bb_log_i(TAG, "OTA push handler registered");
    return ESP_OK;
}

#endif // ESP_PLATFORM
