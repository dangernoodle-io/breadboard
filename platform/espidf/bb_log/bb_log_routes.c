#include "bb_log.h"
#include "bb_http.h"
#include "bb_registry.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb_log_routes";

static volatile TaskHandle_t s_sse_task_handle = NULL;
static volatile int s_sse_client_type = 0;  // 0=none, 1=browser, 2=external

static void sse_task(void *arg)
{
    bb_http_request_t *req = (bb_http_request_t *)arg;

    int fd = bb_http_req_sockfd(req);
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bb_http_resp_set_type(req, "text/event-stream");
    bb_http_resp_set_header(req, "Cache-Control", "no-cache");
    bb_http_resp_set_header(req, "Connection", "keep-alive");
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

    bb_err_t err = bb_http_resp_send_chunk(req, ": connected\n\n", -1);

    char line[192];
    char frame[220];
    /* Send a comment-frame keepalive every ~10s of silence. EventSource
     * ignores `:` lines, but the chunk write surfaces a dead peer immediately
     * so the task can clean up — and the client can use the gap between
     * keepalives to detect a stalled stream. */
    const int idle_ticks_per_ping = 20; /* 20 * 500ms drain timeout = 10s */
    int idle_ticks = 0;
    while (err == BB_OK) {
        size_t n = bb_log_stream_drain(line, sizeof(line), 500);
        if (n == 0) {
            if (++idle_ticks >= idle_ticks_per_ping) {
                err = bb_http_resp_send_chunk(req, ": ping\n\n", -1);
                idle_ticks = 0;
            }
            continue;
        }
        idle_ticks = 0;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        int flen = snprintf(frame, sizeof(frame), "data: %s\n\n", line);
        err = bb_http_resp_send_chunk(req, frame,
                    (flen > 0 && flen < (int)sizeof(frame)) ? flen : -1);
    }

    /* If err != BB_OK the peer is already disconnected; sending another
     * chunk on the dead fd races with httpd's select-loop cleanup of the
     * same fd in call_end_selects, which has crashed in the field
     * (LoadProhibited). Only send the closing chunk while the connection
     * is believed alive. */
    if (err == BB_OK) {
        bb_http_resp_send_chunk(req, NULL, 0);
    }
    bb_http_req_async_handler_complete(req);
    s_sse_task_handle = NULL;
    s_sse_client_type = 0;
    vTaskDelete(NULL);
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
        bb_http_resp_set_type(req, "application/json");
        char body[96];
        int n = snprintf(body, sizeof(body),
            "{\"error\":\"busy\",\"active_client\":\"%s\"}",
            s_sse_client_type == 1 ? "browser" : "external");
        bb_http_resp_send(req, body, n);
        return BB_OK;
    }

    if (client_type == 2) {
        ESP_LOGI(TAG, "external log client connected");
    }

    bb_http_request_t *async_req = NULL;
    if (bb_http_req_async_handler_begin(req, &async_req) != BB_OK) {
        bb_http_resp_send_err(req, 500, "Async init failed");
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
    bb_http_resp_set_type(req, "application/json");
    char buf[96];
    uint32_t dropped = bb_log_stream_dropped_lines();
    if (s_sse_client_type == 0) {
        snprintf(buf, sizeof(buf), "{\"active\":false,\"client\":null,\"dropped\":%" PRIu32 "}", dropped);
    } else {
        snprintf(buf, sizeof(buf), "{\"active\":true,\"client\":\"%s\",\"dropped\":%" PRIu32 "}",
                 s_sse_client_type == 1 ? "browser" : "external", dropped);
    }
    bb_http_resp_sendstr(req, buf);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptors (descriptor-only; handlers registered via bb_http_register_route)
// ---------------------------------------------------------------------------

static const bb_route_response_t s_logs_responses[] = {
    { 200, "text/event-stream", NULL,
      "Server-Sent Events stream of log lines; each event carries one log "
      "line as `data: <line>`. Stream is long-lived; only one client at a "
      "time is supported." },
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
    .method   = BB_HTTP_GET,
    .path     = "/api/logs",
    .tag      = "logs",
    .summary  = "Stream log output via SSE",
    .responses = s_logs_responses,
    .handler  = NULL,  // SSE handler; uses async API
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
    bb_http_register_route_descriptor_only(&s_logs_route);
    bb_http_register_route_descriptor_only(&s_logs_status_route);

    bb_log_i(TAG, "log stream routes registered");
    return BB_OK;
}

BB_REGISTRY_REGISTER_N(bb_log_stream_register_routes, bb_log_stream_register_routes_init, 2);
