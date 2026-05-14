// ESP-IDF route handler for bb_event_routes — registers GET /api/events,
// spawns a FreeRTOS task per client, drains queued events to SSE frames.
#include "bb_event_routes.h"
#include "bb_event_routes_internal.h"
#include "bb_http.h"
#include "bb_log.h"
#include "bb_registry.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bb_event_routes";

// ---------------------------------------------------------------------------
// Port: SemaphoreHandle_t recursive mutex per slot. notify() is unused on
// ESP-IDF because the SSE task polls with a short timeout.
// ---------------------------------------------------------------------------

void *bb_event_routes_port_lock_create(void)
{
    return (void *)xSemaphoreCreateRecursiveMutex();
}

void bb_event_routes_port_lock_destroy(void *lock)
{
    if (lock) vSemaphoreDelete((SemaphoreHandle_t)lock);
}

void bb_event_routes_port_lock(void *lock)
{
    if (lock) xSemaphoreTakeRecursive((SemaphoreHandle_t)lock, portMAX_DELAY);
}

void bb_event_routes_port_unlock(void *lock)
{
    if (lock) xSemaphoreGiveRecursive((SemaphoreHandle_t)lock);
}

void bb_event_routes_port_notify(void *lock) { (void)lock; }

// ---------------------------------------------------------------------------
// Per-client SSE task
// ---------------------------------------------------------------------------

typedef struct {
    bb_http_request_t *req;
    bb_event_routes_client_t *client;
} sse_task_arg_t;

static void sse_task(void *arg)
{
    sse_task_arg_t *t = (sse_task_arg_t *)arg;
    bb_http_request_t *req = t->req;
    bb_event_routes_client_t *client = t->client;
    free(t);

    int fd = bb_http_req_sockfd(req);
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bb_http_resp_set_type(req, "text/event-stream");
    bb_http_resp_set_header(req, "Cache-Control", "no-cache");
    bb_http_resp_set_header(req, "Connection", "keep-alive");
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

    bb_err_t err = bb_http_resp_send_chunk(req, ": connected\nretry: 5000\n\n", -1);

    char frame[CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY + 96];
    const uint32_t hb_ms = bb_event_routes_heartbeat_ms();
    const int idle_ticks_per_ping = hb_ms / 200;
    int idle_ticks = 0;

    while (err == BB_OK) {
        size_t n = bb_event_routes_drain_frame(client, frame, sizeof(frame));
        if (n == 0) {
            if (++idle_ticks >= idle_ticks_per_ping) {
                err = bb_http_resp_send_chunk(req, ": ping\n\n", -1);
                idle_ticks = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        idle_ticks = 0;
        err = bb_http_resp_send_chunk(req, frame, (int)n);
    }

    if (err == BB_OK) {
        bb_http_resp_send_chunk(req, NULL, 0);
    }
    bb_event_routes_client_release(client);
    bb_http_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// HTTP handler
// ---------------------------------------------------------------------------

static bb_err_t events_handler(bb_http_request_t *req)
{
    bb_event_routes_client_t *client = NULL;
    bb_err_t err = bb_event_routes_client_acquire(&client);
    if (err == BB_ERR_NO_SPACE) {
        bb_http_resp_set_status(req, 503);
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_sendstr(req, "{\"error\":\"max_clients\"}");
        return BB_OK;
    }
    if (err != BB_OK) {
        bb_http_resp_send_err(req, 500, "event routes not initialized");
        return err;
    }

    bb_http_request_t *async_req = NULL;
    if (bb_http_req_async_handler_begin(req, &async_req) != BB_OK) {
        bb_event_routes_client_release(client);
        bb_http_resp_send_err(req, 500, "Async init failed");
        return BB_ERR_INVALID_STATE;
    }

    sse_task_arg_t *arg = (sse_task_arg_t *)malloc(sizeof(*arg));
    if (!arg) {
        bb_event_routes_client_release(client);
        bb_http_req_async_handler_complete(async_req);
        return BB_ERR_NO_SPACE;
    }
    arg->req = async_req;
    arg->client = client;

    if (xTaskCreate(sse_task, "sse_events", 4096, arg, 1, NULL) != pdPASS) {
        free(arg);
        bb_event_routes_client_release(client);
        bb_http_req_async_handler_complete(async_req);
        return BB_ERR_INVALID_STATE;
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static const bb_route_response_t s_events_responses[] = {
    { 200, "text/event-stream", NULL,
      "Server-Sent Events stream of bb_event topic posts. Each event has "
      "`event:` (topic name), `data:` (JSON payload posted by the producer), "
      "and `id:` (monotonic per-stream). Topic must have been attached via "
      "bb_event_routes_attach." },
    { 503, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\",\"enum\":[\"max_clients\"]}},"
      "\"required\":[\"error\"]}",
      "maximum concurrent clients reached" },
    { 0 },
};

static const bb_route_t s_events_route = {
    .method = BB_HTTP_GET,
    .path = "/api/events",
    .tag = "events",
    .summary = "Stream bb_event topic posts via SSE",
    .responses = s_events_responses,
    .handler = NULL,
};

static bb_err_t bb_event_routes_register_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t err = bb_event_routes_init(NULL);
    if (err != BB_OK) return err;
    err = bb_http_register_route(server, BB_HTTP_GET, "/api/events", events_handler);
    if (err != BB_OK) return err;
    bb_http_register_route_descriptor_only(&s_events_route);
    bb_log_i(TAG, "registered /api/events");
    return BB_OK;
}

#if CONFIG_BB_EVENT_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER(bb_event_routes, bb_event_routes_register_routes_init);
#endif
