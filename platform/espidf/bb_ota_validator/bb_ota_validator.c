#include "bb_ota_validator.h"

#ifdef ESP_PLATFORM

#include <stdatomic.h>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "bb_http.h"
#include "bb_log.h"
#include "bb_nv.h"

static const char *TAG = "bb_ota_val";

static atomic_bool s_pending     = ATOMIC_VAR_INIT(false);
static atomic_bool s_marked_valid = ATOMIC_VAR_INIT(false);

static bool other_slot_has_valid_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) return false;
    const esp_partition_t *other = esp_ota_get_next_update_partition(running);
    if (other == NULL) return false;
    esp_app_desc_t desc;
    return esp_ota_get_partition_description(other, &desc) == ESP_OK;
}

static void mark_valid_internal(const char *reason)
{
    if (atomic_exchange(&s_marked_valid, true)) {
        return;  // already marked
    }
    esp_ota_mark_app_valid_cancel_rollback();
    bb_nv_config_reset_boot_count();
    bb_log_w(TAG, "firmware validated via %s", reason ? reason : "unknown");
}

bb_err_t bb_ota_mark_valid(const char *reason)
{
    if (!atomic_load(&s_pending) || atomic_load(&s_marked_valid)) {
        return BB_ERR_INVALID_STATE;
    }
    bool was_marked = atomic_exchange(&s_marked_valid, true);
    if (was_marked) {
        return BB_ERR_INVALID_STATE;
    }
    esp_ota_mark_app_valid_cancel_rollback();
    bb_nv_config_reset_boot_count();
    bb_log_w(TAG, "firmware validated via %s", reason ? reason : "unknown");
    return BB_OK;
}

bool bb_ota_is_pending(void)
{
    return atomic_load(&s_pending) && !atomic_load(&s_marked_valid);
}

static bb_err_t mark_valid_handler(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Content-Type", "application/json");

    if (!bb_ota_is_pending()) {
        bb_http_resp_set_status(req, 409);
        static const char body[] = "{\"error\":\"not pending\"}";
        return bb_http_resp_send(req, body, sizeof(body) - 1);
    }

    bb_err_t rc = bb_ota_mark_valid("http");
    if (rc != BB_OK) {
        bb_http_resp_set_status(req, 500);
        static const char body[] = "{\"error\":\"internal\"}";
        return bb_http_resp_send(req, body, sizeof(body) - 1);
    }

    static const char ok_body[] = "{\"status\":\"valid\"}";
    return bb_http_resp_send(req, ok_body, sizeof(ok_body) - 1);
}

bb_err_t bb_ota_validator_init(bb_http_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;

    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (!other_slot_has_valid_app()) {
            bb_log_w(TAG, "other OTA slot lacks a valid app — rollback target unsafe, marking valid immediately");
            mark_valid_internal("rollback-unsafe preflight");
        } else {
            atomic_store(&s_pending, true);
            bb_log_i(TAG, "OTA image pending verification");
        }
    }

    static const bb_route_response_t s_mark_valid_responses[] = {
        { 200, "application/json",
          "{\"type\":\"object\","
          "\"properties\":{\"status\":{\"type\":\"string\"}},"
          "\"required\":[\"status\"]}",
          "firmware marked valid; rollback cancelled" },
        { 409, "application/json",
          "{\"type\":\"object\","
          "\"properties\":{\"error\":{\"type\":\"string\"}},"
          "\"required\":[\"error\"]}",
          "no OTA pending verification" },
        { 0 },
    };

    static const bb_route_t s_mark_valid_route = {
        .method   = BB_HTTP_POST,
        .path     = "/api/ota/mark-valid",
        .tag      = "ota",
        .summary  = "Mark running firmware as valid",
        .responses = s_mark_valid_responses,
        .handler  = mark_valid_handler,
    };

    bb_err_t rc = bb_http_register_described_route(server, &s_mark_valid_route);
    return rc == BB_OK ? ESP_OK : ESP_FAIL;
}

#endif /* ESP_PLATFORM */
