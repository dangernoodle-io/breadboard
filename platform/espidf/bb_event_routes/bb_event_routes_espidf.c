// ESP-IDF route handler for bb_event_routes — registers GET /api/events,
// spawns a FreeRTOS task per client, drains queued events to SSE frames.
// Also registers GET /api/diag/events for topic discovery + ring diagnostics.
#include "bb_event_routes.h"
#include "bb_event_routes_internal.h"
#include "bb_event_ring.h"
#include "bb_http.h"
#include "bb_json.h"
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
    // Parse optional ?topic= query parameter
    char topic_buf[32] = {0};
    const char *topic_filter = NULL;
    if (bb_http_req_query_key_value(req, "topic", topic_buf, sizeof(topic_buf)) == BB_OK) {
        topic_filter = topic_buf;
    }

    bb_event_routes_client_t *client = NULL;
    bb_err_t err = bb_event_routes_client_acquire_ex(&client, topic_filter);
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

// ---------------------------------------------------------------------------
// Diag handler: GET /api/diag/events
// ---------------------------------------------------------------------------

static bb_err_t diag_events_handler(bb_http_request_t *req)
{
    bb_json_t root = bb_json_obj_new();
    if (!root) return bb_http_resp_send_err(req, 500, "JSON alloc failed");

    bb_json_t topics_arr = bb_json_arr_new();
    if (!topics_arr) {
        bb_json_free(root);
        return bb_http_resp_send_err(req, 500, "JSON alloc failed");
    }

    size_t n = bb_event_routes_topic_count();
    for (size_t i = 0; i < n; i++) {
        const char *name = NULL;
        bb_event_ring_t ring = NULL;
        if (bb_event_routes_topic_info(i, &name, &ring) != BB_OK) continue;

        bb_json_t entry = bb_json_obj_new();
        if (!entry) continue;

        bb_json_obj_set_string(entry, "name", name ? name : "");

        if (ring) {
            size_t cap = bb_event_ring_capacity(ring);
            bb_json_obj_set_number(entry, "ring_capacity", (double)cap);
            size_t count = bb_event_ring_count(ring);
            bb_json_obj_set_number(entry, "ring_count", (double)count);

            uint32_t last_id  = 0;
            size_t   last_sz  = 0;
            int64_t  last_us  = 0;
            if (bb_event_ring_last_entry_info(ring, &last_id, &last_sz, &last_us) == BB_OK) {
                bb_json_obj_set_number(entry, "last_id",      (double)last_id);
                bb_json_obj_set_number(entry, "last_post_us", (double)last_us);
                bb_json_obj_set_number(entry, "last_size",    (double)last_sz);
            } else {
                bb_json_obj_set_number(entry, "last_id",      0);
                bb_json_obj_set_number(entry, "last_post_us", 0);
                bb_json_obj_set_number(entry, "last_size",    0);
            }
        } else {
            bb_json_obj_set_number(entry, "ring_capacity", 0);
            bb_json_obj_set_number(entry, "ring_count",    0);
            bb_json_obj_set_number(entry, "last_id",       0);
            bb_json_obj_set_number(entry, "last_post_us",  0);
            bb_json_obj_set_number(entry, "last_size",     0);
        }

        bb_json_arr_append_obj(topics_arr, entry);
    }

    bb_json_obj_set_arr(root, "topics", topics_arr);
    bb_json_obj_set_number(root, "max_clients",    (double)CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS);
    bb_json_obj_set_number(root, "active_clients", (double)bb_event_routes_active_client_count());

    bb_http_resp_set_status(req, 200);
    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

static const bb_route_response_t s_diag_events_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"topics\":{\"type\":\"array\",\"items\":{"
      "\"type\":\"object\","
      "\"properties\":{"
      "\"name\":{\"type\":\"string\"},"
      "\"ring_capacity\":{\"type\":\"integer\"},"
      "\"ring_count\":{\"type\":\"integer\"},"
      "\"last_id\":{\"type\":\"integer\"},"
      "\"last_post_us\":{\"type\":\"integer\"},"
      "\"last_size\":{\"type\":\"integer\"}},"
      "\"required\":[\"name\",\"ring_capacity\",\"ring_count\","
      "\"last_id\",\"last_post_us\",\"last_size\"]}},"
      "\"max_clients\":{\"type\":\"integer\"},"
      "\"active_clients\":{\"type\":\"integer\"}},"
      "\"required\":[\"topics\",\"max_clients\",\"active_clients\"]}",
      "topic discovery and ring-buffer diagnostics for /api/events — "
      "ring_count=0 means no replay data; last_post_us=0 means no events captured yet" },
    { 0 },
};

static const bb_route_t s_diag_events_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/events",
    .tag       = "diag",
    .summary   = "List attached SSE topics with ring-buffer diagnostics",
    .responses = s_diag_events_responses,
    .handler   = diag_events_handler,
};

static bb_err_t bb_event_routes_register_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t err = bb_event_routes_init(NULL);
    if (err != BB_OK) return err;
    err = bb_http_register_route(server, BB_HTTP_GET, "/api/events", events_handler);
    if (err != BB_OK) return err;
    bb_http_register_route_descriptor_only(&s_events_route);
    err = bb_http_register_described_route(server, &s_diag_events_route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "registered /api/events + /api/diag/events");
    return BB_OK;
}

#if CONFIG_BB_EVENT_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER(bb_event_routes, bb_event_routes_register_routes_init);
#endif
