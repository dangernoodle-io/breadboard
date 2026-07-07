#include "bb_ota_validator.h"

#ifdef ESP_PLATFORM

#include <stdatomic.h>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_init.h"

static const char *TAG = "bb_ota_val";

static atomic_bool s_pending     = ATOMIC_VAR_INIT(false);
static atomic_bool s_marked_valid = ATOMIC_VAR_INIT(false);
static void (*s_on_validated_cb)(void) = NULL;

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
    void (*cb)(void) = s_on_validated_cb;
    if (cb) cb();
}

void bb_ota_validator_set_on_validated(void (*cb)(void))
{
    s_on_validated_cb = cb;
}

bool bb_ota_rolled_back(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) return false;
    const esp_partition_t *other = esp_ota_get_next_update_partition(running);
    if (other == NULL) return false;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(other, &st) != ESP_OK) return false;
    return (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID);
}

bb_err_t bb_ota_mark_valid(const char *reason)
{
    if (!atomic_load(&s_pending) || atomic_load(&s_marked_valid)) {
        return BB_ERR_INVALID_STATE;
    }
    mark_valid_internal(reason);
    return BB_OK;
}

bool bb_ota_is_pending(void)
{
    return atomic_load(&s_pending) && !atomic_load(&s_marked_valid);
}

static atomic_bool s_is_validated = ATOMIC_VAR_INIT(false);
static atomic_bool s_validated_cached = ATOMIC_VAR_INIT(false);

bool bb_ota_is_validated(void)
{
    // Cache the result since it's immutable post-boot for the running partition.
    if (atomic_load(&s_validated_cached)) {
        return atomic_load(&s_is_validated);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        atomic_store(&s_validated_cached, true);
        return false;
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        atomic_store(&s_validated_cached, true);
        return false;
    }

    bool result = (ota_state == ESP_OTA_IMG_VALID);
    atomic_store(&s_is_validated, result);
    atomic_store(&s_validated_cached, true);
    return result;
}

static const char *ota_state_str(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:      return "undefined";
    default:                         return "undefined";
    }
}

static bb_err_t partitions_handler(bb_http_request_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    bb_http_json_stream_t arr;
    bb_err_t rc = bb_http_resp_json_arr_begin(req, &arr);
    if (rc != BB_OK) return rc;

    // Iterate all app partitions: factory + ota_0..ota_N
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                      ESP_PARTITION_SUBTYPE_ANY,
                                                      NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p) {
            bb_json_t item = bb_json_obj_new();
            if (item) {
                bb_json_obj_set_string(item, "label",   p->label);
                bb_json_obj_set_number(item, "address", (double)p->address);
                bb_json_obj_set_number(item, "size",    (double)p->size);
                bb_json_obj_set_bool  (item, "running", p == running);

                esp_ota_img_states_t st;
                const char *state_str;
                if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
                    state_str = ota_state_str(st);
                } else {
                    state_str = "undefined";
                }
                bb_json_obj_set_string(item, "state", state_str);

                bb_http_resp_json_arr_emit(&arr, item);
                bb_json_free(item);
            }
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    return bb_http_resp_json_arr_end(&arr);
}

static bb_err_t recover_handler(bb_http_request_t *req)
{
    esp_err_t err = esp_ota_erase_last_boot_app_partition();
    bb_log_i(TAG, "ota recover: erase_last_boot_app -> %s", esp_err_to_name(err));

    bb_http_json_obj_stream_t obj;
    bb_err_t rc = bb_http_resp_json_obj_begin(req, &obj);
    if (rc != BB_OK) return rc;

    if (err == ESP_OK) {
        bb_http_resp_json_obj_set_str(&obj, "status", "recovered");
    } else {
        bb_http_resp_json_obj_set_str(&obj, "status", "nothing_to_recover");
        bb_http_resp_json_obj_set_str(&obj, "detail", esp_err_to_name(err));
    }
    return bb_http_resp_json_obj_end(&obj);
}

static bb_err_t mark_valid_handler(bb_http_request_t *req)
{
    if (!bb_ota_is_pending()) {
        bb_http_resp_set_status(req, 409);
        bb_http_json_obj_stream_t obj;
        bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
        if (err != BB_OK) return err;
        bb_http_resp_json_obj_set_str(&obj, "error", "not pending");
        return bb_http_resp_json_obj_end(&obj);
    }

    bb_err_t rc = bb_ota_mark_valid("http");
    if (rc != BB_OK) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
        if (err != BB_OK) return err;
        bb_http_resp_json_obj_set_str(&obj, "error", "internal");
        return bb_http_resp_json_obj_end(&obj);
    }

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj, "status", "valid");
    return bb_http_resp_json_obj_end(&obj);
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
        { 500, "application/json",
          "{\"type\":\"object\","
          "\"properties\":{\"error\":{\"type\":\"string\"}},"
          "\"required\":[\"error\"]}",
          "bb_ota_mark_valid returned an error (e.g. already marked)" },
        { 0 },
    };

    static const bb_route_t s_mark_valid_route = {
        .method   = BB_HTTP_POST,
        .path     = "/api/update/mark-valid",
        .tag      = "update",
        .summary  = "Mark running firmware as valid",
        .responses = s_mark_valid_responses,
        .handler  = mark_valid_handler,
    };

    static const bb_route_response_t s_partitions_responses[] = {
        { 200, "application/json",
          "{\"type\":\"array\","
          "\"items\":{\"type\":\"object\","
          "\"properties\":{"
          "\"label\":{\"type\":\"string\"},"
          "\"address\":{\"type\":\"integer\"},"
          "\"size\":{\"type\":\"integer\"},"
          "\"running\":{\"type\":\"boolean\"},"
          "\"state\":{\"type\":\"string\"}},"
          "\"required\":[\"label\",\"address\",\"size\",\"running\",\"state\"]}}",
          "list of OTA app partition slot states" },
        { 0 },
    };

    static const bb_route_t s_partitions_route = {
        .method    = BB_HTTP_GET,
        .path      = "/api/update/partitions",
        .tag       = "update",
        .summary   = "List OTA partition slot states",
        .responses = s_partitions_responses,
        .handler   = partitions_handler,
    };

    static const bb_route_response_t s_recover_responses[] = {
        { 200, "application/json",
          "{\"type\":\"object\","
          "\"properties\":{"
          "\"status\":{\"type\":\"string\"},"
          "\"detail\":{\"type\":\"string\"}},"
          "\"required\":[\"status\"]}",
          "slot cleared (recovered) or nothing to recover" },
        { 0 },
    };

    static const bb_route_t s_recover_route = {
        .method    = BB_HTTP_POST,
        .path      = "/api/update/recover",
        .tag       = "update",
        .summary   = "Clear a wedged inactive OTA slot (erase previous boot app)",
        .responses = s_recover_responses,
        .handler   = recover_handler,
    };

    bb_err_t rc = bb_http_register_described_route(server, &s_mark_valid_route);
    if (rc != BB_OK) return BB_ERR_INVALID_STATE;
    rc = bb_http_register_described_route(server, &s_partitions_route);
    if (rc != BB_OK) return BB_ERR_INVALID_STATE;
    rc = bb_http_register_described_route(server, &s_recover_route);
    return rc == BB_OK ? BB_OK : BB_ERR_INVALID_STATE;
}

#if CONFIG_BB_OTA_VALIDATOR_AUTOREGISTER
// PRE_HTTP companion: declare route count before server starts. This was
// previously declared as 1 (the sort-key order value), but bb_ota_validator_init
// registers 3 routes: POST /api/update/mark-valid, GET /api/update/partitions,
// POST /api/update/recover. The undercount was masked only by BB_HTTP_OVERHEAD_SLACK.
static bb_err_t bb_ota_validator_reserve_routes(void)
{
    bb_http_reserve_routes(3);  // POST /api/update/mark-valid + GET /api/update/partitions + POST /api/update/recover
    return BB_OK;
}
BB_INIT_REGISTER_PRE_HTTP(bb_ota_validator, bb_ota_validator_reserve_routes);
BB_INIT_REGISTER_N(bb_ota_validator, bb_ota_validator_init, 1);
#endif

#endif /* ESP_PLATFORM */
