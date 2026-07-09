#include "bb_health.h"
#include "bb_response.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bb_board.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_mdns.h"
#include "bb_wifi_http.h"

#include "../../../components/bb_health/bb_health_schema_priv.h"
#include "../../../components/bb_health/bb_health_stack.h"

// Forward declaration from bb_health_stack.c
bb_err_t bb_health_stack_monitor_init(void);

static const char *TAG = "bb_health";

// File-scope section registry for /api/health.
static bb_response_registry_t s_health_reg = { .tag = "bb_health" };

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_health_register_section(const char *name,
                                     bb_response_get_fn get,
                                     void *ctx,
                                     const char *schema_props)
{
    return bb_response_register(&s_health_reg, name, get, NULL, ctx, schema_props);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// Serialize a bb_json_t tree and stream it via chunked transfer.
static bb_err_t send_json_tree(bb_http_request_t *req, bb_json_t root)
{
    char *str = bb_json_serialize(root);
    if (!str) return BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_set_type(req, "application/json");
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, str, -1);
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, NULL, 0);
    bb_json_free_str(str);
    return err;
}

static bb_err_t health_handler(bb_http_request_t *req)
{
    bb_board_info_t b;
    bb_board_get_info(&b);

    const char *hostname = bb_mdns_get_hostname();

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_bool(root, "ok", bb_health_compute_ok());
    bb_json_obj_set_bool(root, "validated", b.ota_validated);

    // network: status bools/strings only (TA-505) — bb_wifi_emit_status emits
    // only connected/ssid/bssid/ip; no numeric fields.
    bb_json_t net = bb_json_obj_new();
    bb_wifi_emit_status(net);
    if (hostname) {
        bb_json_obj_set_string(net, "mdns", hostname);
    } else {
        bb_json_obj_set_null(net, "mdns");
    }
    bb_json_obj_set_obj(root, "network", net);

    // Sections: mqtt, temp, and any other registered sections.
    bb_response_build_get(&s_health_reg, root);

    bb_err_t err = send_json_tree(req, root);
    bb_json_free(root);
    return err;
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static bb_route_response_t s_health_responses[] = {
    { 200, "application/json",
      NULL,  // filled by bb_response_assemble_schema() at init
      "liveness check" },
    { 0 },
};

static const bb_route_t s_health_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/health",
    .tag       = "health",
    .summary   = "Get liveness status",
    .responses = s_health_responses,
    .handler   = health_handler,
};

bb_err_t bb_health_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    s_health_responses[0].schema = bb_response_freeze_and_assemble(&s_health_reg, k_health_base, k_health_suffix);

    bb_err_t err = bb_http_register_described_route(server, &s_health_route);
    if (err != BB_OK) return err;

    // Start the stack high-water monitor (no-op if FREERTOS_USE_TRACE_FACILITY=n).
    bb_err_t stack_err = bb_health_stack_monitor_init();
    if (stack_err != BB_OK) {
        bb_log_w(TAG, "stack monitor init failed: %d", (int)stack_err);
    }

    bb_log_i(TAG, "health route registered");
    return BB_OK;
}

// PRE_HTTP companion: declare route count before server starts.
bb_err_t bb_health_reserve_routes(void)
{
    bb_http_reserve_routes(1);  // GET /api/health
    return BB_OK;
}
