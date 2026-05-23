#include "bb_http.h"
#include "bb_system.h"
#include "bb_registry.h"


static bb_err_t reboot_handler(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_err_t rc = bb_http_resp_json_obj_begin(req, &obj);
    if (rc != BB_OK) return rc;
    bb_http_resp_json_obj_set_str(&obj, "status", "rebooting");
    rc = bb_http_resp_json_obj_end(&obj);
    bb_system_restart();
    return rc;
}

// ---------------------------------------------------------------------------
// Route descriptors
// ---------------------------------------------------------------------------

static const bb_route_response_t s_reboot_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "reboot acknowledged" },
    { 0 },
};

static const bb_route_t s_reboot_route = {
    .method   = BB_HTTP_POST,
    .path     = "/api/reboot",
    .tag      = "system",
    .summary  = "Reboot the device",
    .responses = s_reboot_responses,
    .handler  = reboot_handler,
};

static bb_err_t bb_system_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t rc = bb_http_register_described_route(server, &s_reboot_route);
    if (rc != BB_OK) return rc;
    return BB_OK;
}

#if CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_system_routes, bb_system_routes_init, 3);
#endif
