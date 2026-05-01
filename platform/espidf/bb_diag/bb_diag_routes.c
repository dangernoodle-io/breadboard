#include "bb_diag.h"
#include "bb_http.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"

#include "esp_system.h"

static const char *TAG = "bb_diag_routes";

static bb_err_t panic_get_handler(bb_http_request_t *req)
{
    bb_json_t root = bb_json_obj_new();
    if (!root) {
        return bb_http_resp_send_err(req, 500, "JSON alloc failed");
    }

    bool available = bb_diag_panic_available();
    bool coredump_avail = bb_diag_panic_coredump_available();
    bb_json_obj_set_bool(root, "available", available);
    if (available || coredump_avail) {
        bb_json_obj_set_number(root, "boots_since", (double)bb_diag_panic_boots_since());
    }

    if (available) {
        // Get reset reason string
        const char *reason_str = "unknown";
        esp_reset_reason_t reason = esp_reset_reason();
        switch (reason) {
            case ESP_RST_PANIC:      reason_str = "panic"; break;
            case ESP_RST_TASK_WDT:   reason_str = "task_wdt"; break;
            case ESP_RST_INT_WDT:    reason_str = "int_wdt"; break;
            case ESP_RST_WDT:        reason_str = "wdt"; break;
            case ESP_RST_BROWNOUT:   reason_str = "brownout"; break;
            default: break;
        }
        bb_json_obj_set_string(root, "reset_reason", reason_str);

        // Retrieve panic log
        char panic_buf[512];
        size_t panic_len = sizeof(panic_buf) - 1;
        if (bb_diag_panic_get(panic_buf, &panic_len) == BB_OK) {
            bb_json_obj_set_string(root, "log_tail", panic_buf);
        }
    }

#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    if (coredump_avail) {
        bb_diag_panic_summary_t summary;
        if (bb_diag_panic_coredump_get(&summary) == BB_OK) {
            bb_json_obj_set_string(root, "task", summary.task_name);
            bb_json_obj_set_number(root, "exc_pc", (double)summary.exc_pc);
            bb_json_obj_set_number(root, "exc_cause", (double)summary.exc_cause);

            bb_json_t bt = bb_json_arr_new();
            for (uint32_t i = 0; i < summary.bt_count; i++) {
                bb_json_arr_append_number(bt, (double)summary.bt_addrs[i]);
            }
            bb_json_obj_set_arr(root, "backtrace", bt);
        }
    }
#endif

    bb_http_resp_set_status(req, 200);
    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

static bb_err_t panic_delete_handler(bb_http_request_t *req)
{
    bb_diag_panic_clear();
    bb_http_resp_set_status(req, 204);
    return bb_http_resp_send(req, NULL, 0);
}

static const bb_route_response_t s_panic_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"available\":{\"type\":\"boolean\"},"
      "\"boots_since\":{\"type\":\"integer\"},"
      "\"reset_reason\":{\"type\":\"string\"},"
      "\"log_tail\":{\"type\":\"string\"},"
      "\"task\":{\"type\":\"string\"},"
      "\"exc_pc\":{\"type\":\"integer\"},"
      "\"exc_cause\":{\"type\":\"integer\"},"
      "\"backtrace\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}},"
      "\"required\":[\"available\"]}",
      "panic log status, log tail, and coredump backtrace (when available)" },
    { 0 },
};

static const bb_route_t s_panic_get_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/diag/panic",
    .tag      = "diag",
    .summary  = "Get panic log from previous abnormal boot",
    .responses = s_panic_get_responses,
    .handler  = panic_get_handler,
};

static const bb_route_response_t s_panic_delete_responses[] = {
    { 204, NULL, NULL, "panic log cleared" },
    { 0 },
};

static const bb_route_t s_panic_delete_route = {
    .method   = BB_HTTP_DELETE,
    .path     = "/api/diag/panic",
    .tag      = "diag",
    .summary  = "Clear panic log",
    .responses = s_panic_delete_responses,
    .handler  = panic_delete_handler,
};

#ifdef CONFIG_BB_DIAG_PANIC_TRIGGER
static bb_err_t panic_trigger_handler(bb_http_request_t *req)
{
    (void)req;
    volatile int *p = NULL;
    *p = 0;
    return BB_OK;
}

static const bb_route_response_t s_panic_trigger_responses[] = {
    { 500, NULL, NULL, "never returned — handler panics before sending response" },
    { 0 },
};

static const bb_route_t s_panic_trigger_route = {
    .method    = BB_HTTP_POST,
    .path      = "/api/diag/panic/trigger",
    .tag       = "diag",
    .summary   = "Force a panic via null dereference (debug builds only)",
    .responses = s_panic_trigger_responses,
    .handler   = panic_trigger_handler,
};
#endif

// /api/info extender: adds an optional "panic" object only when a panic
// log or coredump is present, so clean boots see no schema change.
static void bb_diag_info_extender(bb_json_t root)
{
    bool avail = bb_diag_panic_available();
    bool coredump = bb_diag_panic_coredump_available();
    if (!avail && !coredump) return;

    bb_json_t panic = bb_json_obj_new();
    bb_json_obj_set_bool(panic, "available", avail);
    bb_json_obj_set_bool(panic, "coredump", coredump);
    bb_json_obj_set_number(panic, "boots_since", (double)bb_diag_panic_boots_since());
    bb_json_obj_set_obj(root, "panic", panic);
}

static bb_err_t bb_diag_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_described_route(server, &s_panic_get_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_panic_delete_route);
    if (err != BB_OK) return err;

#ifdef CONFIG_BB_DIAG_PANIC_TRIGGER
    err = bb_http_register_described_route(server, &s_panic_trigger_route);
    if (err != BB_OK) return err;
    bb_log_w(TAG, "panic trigger route ENABLED — debug build only");
#endif

    bb_info_register_extender(bb_diag_info_extender);

    bb_log_i(TAG, "panic routes + info extender registered");
    return BB_OK;
}

BB_REGISTRY_REGISTER_N(bb_diag_routes, bb_diag_routes_init, 4);
