#include "ota_validator.h"

#ifdef ESP_PLATFORM

#include <stdatomic.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "http_server.h"

static const char *TAG = "bb_ota_val";

static atomic_bool s_marked_valid = ATOMIC_VAR_INIT(false);

static bool running_is_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return false;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool bb_ota_is_pending(void)
{
    if (atomic_load(&s_marked_valid)) return false;
    return running_is_pending_verify();
}

bb_err_t bb_ota_mark_valid_manual(void)
{
    if (atomic_exchange(&s_marked_valid, true)) {
        return BB_ERR_INVALID_ARG;
    }
    if (!running_is_pending_verify()) {
        atomic_store(&s_marked_valid, false);
        return BB_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        atomic_store(&s_marked_valid, false);
        ESP_LOGE(TAG, "mark-valid failed: %d", err);
        return BB_ERR_INVALID_ARG;
    }
    ESP_LOGW(TAG, "firmware validated via manual override");
    return BB_OK;
}

static bb_err_t mark_valid_handler(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Content-Type", "application/json");
    if (!running_is_pending_verify() || atomic_load(&s_marked_valid)) {
        bb_http_resp_set_status(req, 409);
        static const char body[] = "{\"error\":\"not pending\"}";
        return bb_http_resp_send(req, body, sizeof(body) - 1);
    }
    bb_err_t rc = bb_ota_mark_valid_manual();
    if (rc != BB_OK) {
        bb_http_resp_set_status(req, 500);
        static const char body[] = "{\"error\":\"internal\"}";
        return bb_http_resp_send(req, body, sizeof(body) - 1);
    }
    static const char ok_body[] = "{\"status\":\"valid\"}";
    return bb_http_resp_send(req, ok_body, sizeof(ok_body) - 1);
}

esp_err_t bb_ota_register_validator_route(bb_http_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    bb_err_t rc = bb_http_register_route(server, BB_HTTP_POST, "/api/ota/mark-valid", mark_valid_handler);
    return rc == BB_OK ? ESP_OK : ESP_FAIL;
}

#endif /* ESP_PLATFORM */
