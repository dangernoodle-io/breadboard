#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_system.h"
#include "bb_init.h"

// Bound on the request body accepted for POST /api/reboot's optional
// {"ts": <epoch_s>, "detail": "<string>"} JSON. Both fields are optional;
// no body at all is a normal, tolerated request.
#define BB_SYSTEM_REBOOT_BODY_MAX 256

static bb_err_t reboot_handler(bb_http_request_t *req)
{
    // Read the (optional) body, bounded and NUL-terminated.
    char body[BB_SYSTEM_REBOOT_BODY_MAX + 1];
    int  body_len = 0;
    int  raw_len  = bb_http_req_body_len(req);
    if (raw_len > 0 && raw_len <= BB_SYSTEM_REBOOT_BODY_MAX) {
        int n = bb_http_req_recv(req, body, sizeof(body) - 1);
        if (n > 0) {
            body[n] = '\0';
            body_len = n;
        }
    }

    // Resolve the User-Agent fallback once, up front — the pure parse fn is
    // request-independent and takes the already-resolved string.
    char ua[49];
    const char *ua_p = (bb_http_req_get_header(req, "User-Agent", ua, sizeof(ua)) == BB_OK) ? ua : NULL;

    uint32_t ts = 0;
    char detail[49] = {0};
    bb_system_reboot_parse_body(body_len > 0 ? body : NULL, body_len, ua_p, &ts, detail, sizeof(detail));

    bb_http_json_obj_stream_t obj;
    bb_err_t rc = bb_http_resp_json_obj_begin(req, &obj);
    if (rc != BB_OK) return rc;
    bb_http_resp_json_obj_set_str(&obj, "status", "rebooting");
    rc = bb_http_resp_json_obj_end(&obj);
    bb_system_restart_reason_at(BB_RESET_SRC_API_REBOOT, detail[0] ? detail : NULL, ts);
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
static bb_err_t bb_system_routes_reserve(void)
{
    bb_http_reserve_routes(1);  // POST /api/reboot
    return BB_OK;
}
BB_INIT_REGISTER_PRE_HTTP(bb_system_routes, bb_system_routes_reserve);
BB_INIT_REGISTER_N(bb_system_routes, bb_system_routes_init, 3);
#endif
