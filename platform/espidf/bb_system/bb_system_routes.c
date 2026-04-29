#include "bb_http.h"
#include "bb_system.h"
#include "bb_registry.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#include "esp_timer.h"

static bb_err_t version_handler(bb_http_request_t *req)
{
    const char *version = bb_system_get_version();
    bb_http_resp_set_type(req, "text/plain");
    return bb_http_resp_send(req, version, strlen(version));
}

static bb_err_t reboot_handler(bb_http_request_t *req)
{
    static const char body[] = "{\"status\":\"rebooting\"}";
    bb_http_resp_set_type(req, "application/json");
    bb_err_t rc = bb_http_resp_send(req, body, sizeof(body) - 1);
    bb_system_restart();
    return rc;
}

static bb_err_t ping_handler(bb_http_request_t *req)
{
    char body[32];
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    int n = snprintf(body, sizeof(body), "ok %" PRIu32, uptime_s);
    if (n < 0 || (size_t)n >= sizeof(body)) n = 2;  /* safe fallback to "ok" */
    bb_http_resp_set_type(req, "text/plain");
    return bb_http_resp_send(req, body, (size_t)n);
}

// ---------------------------------------------------------------------------
// Route descriptors
// ---------------------------------------------------------------------------

static const bb_route_response_t s_version_responses[] = {
    { 200, "text/plain", NULL, "firmware version string" },
    { 0 },
};

static const bb_route_t s_version_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/version",
    .tag      = "system",
    .summary  = "Get firmware version",
    .responses = s_version_responses,
    .handler  = version_handler,
};

static const bb_route_response_t s_ping_responses[] = {
    { 200, "text/plain", NULL, "ok <uptime_seconds>" },
    { 0 },
};

static const bb_route_t s_ping_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/ping",
    .tag      = "system",
    .summary  = "Liveness check",
    .responses = s_ping_responses,
    .handler  = ping_handler,
};

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

    bb_err_t rc;
    rc = bb_http_register_described_route(server, &s_version_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_ping_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_reboot_route);
    if (rc != BB_OK) return rc;
    return BB_OK;
}

#if CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_system_routes, bb_system_routes_init, 3);
#endif
