#include "bb_log.h"
#include "bb_http.h"
#include "bb_registry.h"
#include "bb_sse_writer.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb_log_routes";

static volatile TaskHandle_t s_sse_task_handle = NULL;
static volatile int s_sse_client_type = 0;  // 0=none, 1=browser, 2=external

// ---------------------------------------------------------------------------
// SSE writer callbacks
// ---------------------------------------------------------------------------

static int log_wait_fn(void *ctx, char *buf, size_t buflen, uint32_t timeout_ms)
{
    (void)ctx;
    char line[192];
    size_t n = bb_log_stream_drain(line, sizeof(line), timeout_ms);
    if (n == 0) return 0;  // idle timeout
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    int flen = snprintf(buf, buflen, "data: %s\n\n", line);
    return (flen > 0 && flen < (int)buflen) ? flen : -1;
}

static void log_cleanup_fn(void *ctx)
{
    (void)ctx;
    s_sse_task_handle = NULL;
    s_sse_client_type = 0;
}

static void sse_task(void *arg)
{
    bb_http_request_t *req = (bb_http_request_t *)arg;
    bb_sse_writer_run(req, ": connected\n\n",
                      log_wait_fn, log_cleanup_fn, NULL,
                      500,
                      CONFIG_BB_LOG_SSE_KEEPALIVE_MS);
    // bb_sse_writer_run calls vTaskDelete(NULL) — never returns.
}

static bb_err_t logs_handler(bb_http_request_t *req)
{
    int client_type = 2;
    char val[16];
    if (bb_http_req_query_key_value(req, "source", val, sizeof(val)) == BB_OK
        && strcmp(val, "browser") == 0) {
        client_type = 1;
    }

    if (s_sse_task_handle) {
        bb_http_resp_set_status(req, 503);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "busy");
        bb_http_resp_json_obj_set_str(&obj, "active_client",
                                      s_sse_client_type == 1 ? "browser" : "external");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    if (client_type == 2) {
        bb_log_i(TAG, "external log client connected");
    }

    bb_http_request_t *async_req = NULL;
    if (bb_http_req_async_handler_begin(req, &async_req) != BB_OK) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "Async init failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }

    s_sse_client_type = client_type;
    if (xTaskCreate(sse_task, "sse_log", 4096, async_req, 1, (TaskHandle_t *)&s_sse_task_handle) != pdPASS) {
        bb_http_req_async_handler_complete(async_req);
        s_sse_client_type = 0;
        return BB_ERR_INVALID_STATE;
    }
    return BB_OK;
}

static bb_err_t logs_status_handler(bb_http_request_t *req)
{
    uint32_t dropped = bb_log_stream_dropped_lines();
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_bool(&obj, "active", s_sse_client_type != 0);
    if (s_sse_client_type == 0) {
        bb_http_resp_json_obj_set_null(&obj, "client");
    } else {
        bb_http_resp_json_obj_set_str(&obj, "client",
                                      s_sse_client_type == 1 ? "browser" : "external");
    }
    bb_http_resp_json_obj_set_int(&obj, "dropped", (int64_t)dropped);
    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Route descriptors (descriptor-only; handlers registered via bb_http_register_route)
// ---------------------------------------------------------------------------

static const bb_route_param_t s_logs_params[] = {
    {
        .name        = "source",
        .in          = "query",
        .description = "Set to 'browser' to identify this client as a browser-originated "
                       "connection; any other value (or omitted) is treated as an external client.",
        .required    = false,
        .schema_type = "string",
    },
};

static const bb_route_response_t s_logs_responses[] = {
    { 200, "text/event-stream", NULL,
      "Server-Sent Events stream of log lines; each event carries one log "
      "line as `data: <line>`. Stream is long-lived; only one client at a "
      "time is supported." },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "async handler init failed or task create failed" },
    { 503, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"error\":{\"type\":\"string\",\"enum\":[\"busy\"]},"
      "\"active_client\":{\"type\":\"string\",\"enum\":[\"browser\",\"external\"]}},"
      "\"required\":[\"error\",\"active_client\"]}",
      "another client already has the log stream open" },
    { 0 },
};

static const bb_route_t s_logs_route = {
    .method           = BB_HTTP_GET,
    .path             = "/api/logs",
    .tag              = "logs",
    .summary          = "Stream log output via SSE",
    .responses        = s_logs_responses,
    .parameters       = s_logs_params,
    .parameters_count = 1,
    .handler          = NULL,  // SSE handler; uses async API
};

static const bb_route_response_t s_logs_status_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"active\":{\"type\":\"boolean\"},"
      "\"client\":{\"type\":[\"string\",\"null\"],"
      "\"enum\":[\"browser\",\"external\",null]},"
      "\"dropped\":{\"type\":\"integer\"}},"
      "\"required\":[\"active\",\"client\",\"dropped\"]}",
      "current SSE stream status" },
    { 0 },
};

static const bb_route_t s_logs_status_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/logs/status",
    .tag      = "logs",
    .summary  = "Get log stream status",
    .responses = s_logs_status_responses,
    .handler  = NULL,
};

static bb_err_t bb_log_stream_register_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_route(server, BB_HTTP_GET,
                                          "/api/logs", logs_handler);
    if (err != BB_OK) return err;
    err = bb_http_register_route(server, BB_HTTP_GET,
                                 "/api/logs/status", logs_status_handler);
    if (err != BB_OK) return err;

    // Register descriptors for OpenAPI spec; handlers are already registered above.
    bb_err_t desc_err = bb_http_register_route_descriptor_only(&s_logs_route);
    if (desc_err != BB_OK) {
        bb_log_e(TAG, "failed to register logs descriptor: %d", desc_err);
    }
    desc_err = bb_http_register_route_descriptor_only(&s_logs_status_route);
    if (desc_err != BB_OK) {
        bb_log_e(TAG, "failed to register logs-status descriptor: %d", desc_err);
    }

    bb_log_i(TAG, "log stream routes registered");
    return BB_OK;
}

// PRE_HTTP companion: declare route count before server starts (must match
// the number of bb_http_register_route() calls in bb_log_stream_register_routes_init: 2).
// Note: bb_http_register_route_descriptor_only() does NOT register a handler
// slot, so only the two bb_http_register_route() calls count here.
static bb_err_t bb_log_stream_register_routes_reserve(void)
{
    bb_http_reserve_routes(2);  // GET /api/logs + GET /api/logs/status
    return BB_OK;
}
BB_REGISTRY_REGISTER_PRE_HTTP(bb_log_stream_register_routes, bb_log_stream_register_routes_reserve);
BB_REGISTRY_REGISTER_N(bb_log_stream_register_routes, bb_log_stream_register_routes_init, 2);
