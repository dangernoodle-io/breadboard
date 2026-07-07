// ESP-IDF route handler for bb_ws_server — registers GET /api/diag/websocket.
// Mirrors bb_event_routes_espidf's GET /api/diag/events: each component owns
// its own diag route rather than bb_diag core owning it.
//
// Compiled unconditionally under ESP_PLATFORM (independent of
// CONFIG_HTTPD_WS_SUPPORT — bb_ws_server_open_count() returns 0 when WS
// support is compiled out, so the route still reports a valid, if trivial,
// snapshot instead of failing to exist).
#ifdef ESP_PLATFORM
#include "sdkconfig.h"

#include "bb_ws_server.h"
#include "bb_http.h"
#include "bb_init.h"
#include "bb_log.h"

static const char *TAG = "bb_ws_diag";

static bb_err_t ws_diag_handler(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_int(&obj, "open_connections",
                                   (int64_t)bb_ws_server_open_count());

    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_diag_websocket_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"open_connections\":{\"type\":\"integer\"}},"
      "\"required\":[\"open_connections\"]}",
      "current count of open WebSocket connections across all registered "
      "endpoints" },
    { 0 },
};

static const bb_route_t s_diag_websocket_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/websocket",
    .tag       = "diag",
    .summary   = "Report the current open WebSocket connection count",
    .responses = s_diag_websocket_responses,
    .handler   = ws_diag_handler,
};

static bb_err_t bb_ws_server_diag_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t err = bb_http_register_described_route(server, &s_diag_websocket_route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "registered /api/diag/websocket");
    return BB_OK;
}

#if CONFIG_BB_WS_SERVER_DIAG_AUTOREGISTER
BB_INIT_REGISTER(bb_ws_server_diag, bb_ws_server_diag_init);
#endif

#endif // ESP_PLATFORM
