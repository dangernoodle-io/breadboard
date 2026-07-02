// ESP-IDF route handler for bb_event_routes — registers GET /api/events,
// spawns a FreeRTOS task per client, drains queued events to SSE frames.
// Also registers GET /api/diag/events for topic discovery + ring diagnostics.
#include "bb_event_routes.h"
#include "bb_event_routes_internal.h"
#include "bb_event_ring.h"
#include "bb_http.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "bb_init.h"
#include "bb_sse_writer.h"
#include "bb_timer.h"
#include "bb_task_registry.h"
#include "bb_arena.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bb_event_routes";

// ---------------------------------------------------------------------------
// Static task stacks (CONFIG_BB_EVENT_ROUTES_STATIC_POOL) — B1-478 PR E
// (redesigned post-review: index-addressed bb_arena array, no bb_pool)
//
// Pre-allocated per-client bundle {stack, TCB} for xTaskCreateStatic, as a
// fixed array carved once from an ESP-IDF-only, SSE-private bb_arena
// (separate from the portable payload/entry array in
// bb_event_routes_common.c — StackType_t/StaticTask_t are FreeRTOS types
// and cannot appear in that portable, host-compiled file). Client slot i
// (the same index the CAS client-slot loop in bb_event_routes_common.c
// assigns atomically, recovered here via bb_event_routes_client_slot_index)
// deterministically owns s_sse_task_bundles[i] for the arena's lifetime —
// no acquire/release, no free-list. Same fixed BSS reservation as the
// previous bespoke s_sse_stack/s_sse_tcb arrays.
// ---------------------------------------------------------------------------

#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL
#define SSE_TASK_STACK_WORDS (4096 / sizeof(StackType_t))

typedef struct {
    StackType_t  stack[SSE_TASK_STACK_WORDS];
    StaticTask_t tcb;
} sse_task_bundle_t;

// xTaskCreateStatic requires the stack buffer and TCB to be at least
// naturally aligned for their respective types; bb_arena_alloc() returns
// _Alignof(max_align_t)-aligned storage, which is >= both — verify that
// assumption holds on this target rather than relying on it silently.
_Static_assert(_Alignof(max_align_t) >= _Alignof(StackType_t) &&
               _Alignof(max_align_t) >= _Alignof(StaticTask_t),
               "bb_arena alignment insufficient for FreeRTOS static task storage");

// Documented flat allowance covering bb_arena's internal header struct
// (private to bb_arena.c) plus max_align_t rounding of the single
// bb_arena_alloc() call below — see the matching comment in
// bb_event_routes_common.c. Not a magic constant: the bulk of the buffer is
// computed from MAX_CLIENTS * sizeof(sse_task_bundle_t).
#define SSE_TASK_ARENA_HDR_ALLOWANCE_BYTES 128u
#define SSE_TASK_BUNDLES_BYTES \
    ((size_t)CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS * sizeof(sse_task_bundle_t))

_Static_assert(CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS == 0 ||
               (SSE_TASK_BUNDLES_BYTES / CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS) == sizeof(sse_task_bundle_t),
               "SSE task bundle byte count overflowed size_t");

static uint8_t s_sse_task_arena_buf[
    SSE_TASK_BUNDLES_BYTES + SSE_TASK_ARENA_HDR_ALLOWANCE_BYTES
] __attribute__((aligned(_Alignof(max_align_t))));
static bb_arena_t s_sse_task_arena;
static sse_task_bundle_t *s_sse_task_bundles;  // [MAX_CLIENTS], index == client slot

// Eagerly creates the SSE-private task-bundle arena + fixed bundle array.
// Called once from bb_event_routes_register_routes_init. The backing buffer
// is compile-time sized to always fit (see the static asserts above), so
// both calls are provably unreachable-on-failure — assert() rather than a
// branch + return, so there is no uncovered error branch to test.
static void sse_task_bundles_init(void)
{
    bb_err_t err = bb_arena_init(&s_sse_task_arena, s_sse_task_arena_buf,
                                  sizeof(s_sse_task_arena_buf));
    assert(err == BB_OK);
    (void)err;  // avoid unused-variable warning when NDEBUG compiles the assert out
    s_sse_task_bundles = (sse_task_bundle_t *)bb_arena_alloc(s_sse_task_arena, SSE_TASK_BUNDLES_BYTES);
    assert(s_sse_task_bundles != NULL);
}
#endif

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

// Binary semaphore: capture_cb signals after enqueue; sse_task waits with a
// heartbeat-sized timeout so it wakes immediately on new events and emits a
// keepalive ping otherwise — no polling, no extra latency.
void *bb_event_routes_port_event_create(void)
{
    return (void *)xSemaphoreCreateBinary();
}

void bb_event_routes_port_event_destroy(void *event)
{
    if (event) vSemaphoreDelete((SemaphoreHandle_t)event);
}

void bb_event_routes_port_event_signal(void *event)
{
    if (event) xSemaphoreGive((SemaphoreHandle_t)event);
}

bool bb_event_routes_port_event_wait(void *event, uint32_t timeout_ms)
{
    if (!event) return false;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake((SemaphoreHandle_t)event, ticks) == pdTRUE;
}

// ---------------------------------------------------------------------------
// Per-client SSE task
// ---------------------------------------------------------------------------

typedef struct {
    bb_http_request_t *req;
    bb_event_routes_client_t *client;
    void *event;
    bool has_more;  // drain one frame; more may be queued
} sse_task_arg_t;

// wait_fn: event-driven drain. Waits on the per-client signal up to timeout_ms,
// then drains one frame. has_more=true skips the wait and drains directly so
// all queued frames are flushed without counting spurious idle time.
static int events_wait_fn(void *ctx, char *buf, size_t buflen, uint32_t timeout_ms)
{
    sse_task_arg_t *t = (sse_task_arg_t *)ctx;

    if (t->has_more) {
        size_t n = bb_event_routes_drain_frame(t->client, buf, buflen);
        if (n > 0) return (int)n;
        t->has_more = false;
        // Queue exhausted — fall through to a real wait.
    }

    bool signaled = bb_event_routes_port_event_wait(t->event, timeout_ms);
    if (!signaled) return 0;  // idle timeout

    size_t n = bb_event_routes_drain_frame(t->client, buf, buflen);
    if (n == 0) return 0;  // signaled but nothing queued
    t->has_more = true;    // assume more frames; drain next call
    return (int)n;
}

// cleanup_fn: deregister this task, release the client slot, and free the
// arg struct. Runs on the SSE task itself, immediately before bb_sse_writer_run
// calls vTaskDelete(NULL) — xTaskGetCurrentTaskHandle() is still valid here and
// matches the handle registered at task-create time (per-client unique name).
// Deregister MUST happen before client_release: release() sets the client
// slot's in_use=false, which makes the slot (and its "sse_events_<slot>" name)
// immediately reusable by a concurrent acquire on the other core. Releasing
// first would let that new connection's task register the same name while
// this task's stale registry entry still exists — BB_ERR_DUPLICATE races.
static void events_cleanup_fn(void *ctx)
{
    sse_task_arg_t *t = (sse_task_arg_t *)ctx;
    bb_err_t drc = bb_task_registry_deregister(xTaskGetCurrentTaskHandle());
    if (drc != BB_OK) {
        // Benign/expected — e.g. BB_ERR_NOT_FOUND when the earlier register
        // call failed. Debug-level only; not actionable at runtime.
        bb_log_d(TAG, "sse task deregister: %d", drc);
    }
    // Static-pool task bundles (stack + TCB) are index-owned by the client
    // slot (see sse_task_bundles_init doc comment above) — nothing to
    // release here; freeing the client slot below is what makes the slot
    // (and its bundle) reusable by the next connection on that index.
    bb_event_routes_client_release(t->client);
    bb_mem_free(t);
}

// Registers the newly-created SSE task in the task registry. Failure is
// non-fatal (the task still serves the client) but logged for diagnosability.
static void sse_task_registry_register_or_warn(const char *name, uint32_t stack_bytes, TaskHandle_t th)
{
    bb_err_t rc = bb_task_registry_register(name, stack_bytes, th, NULL, NULL);
    if (rc != BB_OK) {
        bb_log_w(TAG, "sse task registry register failed: %d", rc);
    }
}

static void sse_task(void *arg)
{
    sse_task_arg_t *t = (sse_task_arg_t *)arg;
    bb_http_request_t *req = t->req;
    t->event = bb_event_routes_client_event(t->client);
    t->has_more = false;
    const uint32_t hb_ms = bb_event_routes_heartbeat_ms();
    bb_sse_writer_run(req, ": connected\nretry: 5000\n\n",
                      events_wait_fn, events_cleanup_fn, t,
                      hb_ms, hb_ms);
    // bb_sse_writer_run calls vTaskDelete(NULL) — never returns.
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
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "max_clients");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }
    if (err != BB_OK) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "event routes not initialized");
        bb_http_resp_json_obj_end(&obj);
        return err;
    }

    bb_http_request_t *async_req = NULL;
    if (bb_http_req_async_handler_begin(req, &async_req) != BB_OK) {
        bb_event_routes_client_release(client);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "Async init failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }

    sse_task_arg_t *arg = (sse_task_arg_t *)bb_malloc_prefer_spiram(sizeof(*arg));
    if (!arg) {
        bb_event_routes_client_release(client);
        bb_http_req_async_handler_complete(async_req);
        return BB_ERR_NO_SPACE;
    }
    arg->req = async_req;
    arg->client = client;

    // Each concurrent SSE client gets a unique registry name keyed off its
    // client slot index — a literal "sse_events" for every client would collide
    // in the registry (dup-name rejection hides all but the first). Slot index
    // is bounded 0..BB_EVENT_ROUTES_MAX_CLIENTS-1, so the name stays well under
    // configMAX_TASK_NAME_LEN (typically 16).
    char task_name[BB_TASK_REGISTRY_NAME_MAX];
    int slot = bb_event_routes_client_slot_index(client);
    snprintf(task_name, sizeof(task_name), "sse_events_%d", slot);

#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL
    {
        // Index-addressed: client slot `slot` deterministically owns
        // s_sse_task_bundles[slot] for the arena's lifetime (see
        // sse_task_bundles_init doc comment above) — no acquire/release, no
        // free-list. `slot` is bounded 0..MAX_CLIENTS-1 by the CAS
        // client-slot claim in bb_event_routes_client_acquire_ex, so the
        // index is always valid here.
        sse_task_bundle_t *bundle = &s_sse_task_bundles[slot];
        TaskHandle_t th = xTaskCreateStatic(sse_task, task_name,
                                            SSE_TASK_STACK_WORDS, arg, 1,
                                            bundle->stack,
                                            &bundle->tcb);
        if (!th) {
            bb_mem_free(arg);
            bb_event_routes_client_release(client);
            bb_http_req_async_handler_complete(async_req);
            return BB_ERR_INVALID_STATE;
        }
        sse_task_registry_register_or_warn(task_name, SSE_TASK_STACK_WORDS * sizeof(StackType_t), th);
    }
#else
    {
        TaskHandle_t th = NULL;
        if (xTaskCreate(sse_task, task_name, 4096, arg, 1, &th) != pdPASS) {
            bb_mem_free(arg);
            bb_event_routes_client_release(client);
            bb_http_req_async_handler_complete(async_req);
            return BB_ERR_INVALID_STATE;
        }
        sse_task_registry_register_or_warn(task_name, 4096, th);
    }
#endif
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
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "event routes not initialized, or async handler init failed" },
    { 503, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\",\"enum\":[\"max_clients\"]}},"
      "\"required\":[\"error\"]}",
      "maximum concurrent clients reached" },
    { 0 },
};

static const bb_route_param_t s_events_params[] = {
    {
        .name        = "topic",
        .in          = "query",
        .description = "Filter the SSE stream to a single topic name. Available topics are "
                       "listed by GET /api/diag/events. Omit to receive all attached topics.",
        .required    = false,
        .schema_type = "string",
    },
};

static const bb_route_t s_events_route = {
    .method            = BB_HTTP_GET,
    .path              = "/api/events",
    .tag               = "events",
    .summary           = "Stream bb_event topic posts via SSE",
    .responses         = s_events_responses,
    .handler           = events_handler,
    .parameters        = s_events_params,
    .parameters_count  = 1,
};

// ---------------------------------------------------------------------------
// Diag handler: GET /api/diag/events
// ---------------------------------------------------------------------------

static bb_err_t diag_events_handler(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    // Capture now_us once for age computation across all topics.
    int64_t now_us = (int64_t)bb_timer_now_us();

    // "topics" array
    bb_http_resp_json_obj_set_arr_begin(&obj, "topics");
    size_t n = bb_event_routes_topic_count();
    for (size_t i = 0; i < n; i++) {
        const char *name = NULL;
        bb_event_ring_t ring = NULL;
        if (bb_event_routes_topic_info(i, &name, &ring) != BB_OK) continue;

        bb_http_resp_json_obj_set_obj_begin(&obj, NULL);
        bb_http_resp_json_obj_set_str(&obj, "name", name ? name : "");

        if (ring) {
            bb_http_resp_json_obj_set_int(&obj, "ring_capacity", (int64_t)bb_event_ring_capacity(ring));
            bb_http_resp_json_obj_set_int(&obj, "ring_count",    (int64_t)bb_event_ring_count(ring));

            uint32_t last_id = 0;
            size_t   last_sz = 0;
            int64_t  last_us = 0;
            if (bb_event_ring_last_entry_info(ring, &last_id, &last_sz, &last_us) == BB_OK) {
                int64_t age_ms = (last_us > 0 && now_us >= last_us)
                                 ? (now_us - last_us) / 1000
                                 : 0;
                bb_http_resp_json_obj_set_int(&obj, "last_id",          (int64_t)last_id);
                bb_http_resp_json_obj_set_int(&obj, "last_post_age_ms", age_ms);
                bb_http_resp_json_obj_set_int(&obj, "last_size",        (int64_t)last_sz);
            } else {
                bb_http_resp_json_obj_set_int(&obj, "last_id",          0);
                bb_http_resp_json_obj_set_int(&obj, "last_post_age_ms", 0);
                bb_http_resp_json_obj_set_int(&obj, "last_size",        0);
            }
        } else {
            bb_http_resp_json_obj_set_int(&obj, "ring_capacity",    0);
            bb_http_resp_json_obj_set_int(&obj, "ring_count",       0);
            bb_http_resp_json_obj_set_int(&obj, "last_id",          0);
            bb_http_resp_json_obj_set_int(&obj, "last_post_age_ms", 0);
            bb_http_resp_json_obj_set_int(&obj, "last_size",        0);
        }
        bb_http_resp_json_obj_set_obj_end(&obj);
    }
    bb_http_resp_json_obj_set_arr_end(&obj);

    bb_http_resp_json_obj_set_int(&obj, "max_clients",    (int64_t)CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS);
    bb_http_resp_json_obj_set_int(&obj, "active_clients", (int64_t)bb_event_routes_active_client_count());

    return bb_http_resp_json_obj_end(&obj);
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
      "\"last_post_age_ms\":{\"type\":\"integer\"},"
      "\"last_size\":{\"type\":\"integer\"}},"
      "\"required\":[\"name\",\"ring_capacity\",\"ring_count\","
      "\"last_id\",\"last_post_age_ms\",\"last_size\"]}},"
      "\"max_clients\":{\"type\":\"integer\"},"
      "\"active_clients\":{\"type\":\"integer\"}},"
      "\"required\":[\"topics\",\"max_clients\",\"active_clients\"]}",
      "topic discovery and ring-buffer diagnostics for /api/events — "
      "ring_count=0 means no replay data; last_post_age_ms=0 means no events captured yet" },
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

// Forward declaration: implemented in bb_event_routes_spiram.c (same component).
// Sets SPIRAM-preferred allocator for per-client queue buffers before any
// client slot is allocated.
void bb_event_routes_spiram_init(void);

static bb_err_t bb_event_routes_register_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_event_routes_spiram_init();
#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL
    sse_task_bundles_init();
#endif
    bb_err_t err = bb_event_routes_init(NULL);
    if (err != BB_OK) return err;
    err = bb_http_register_described_route(server, &s_events_route);
    if (err != BB_OK) return err;
    err = bb_http_register_described_route(server, &s_diag_events_route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "registered /api/events + /api/diag/events");
    return BB_OK;
}

#if CONFIG_BB_EVENT_ROUTES_AUTOREGISTER
static bb_err_t bb_event_routes_reserve_routes(void)
{
    bb_http_reserve_routes(2);  // GET /api/events + GET /api/diag/events
    return BB_OK;
}
BB_INIT_REGISTER_PRE_HTTP(bb_event_routes, bb_event_routes_reserve_routes);
BB_INIT_REGISTER(bb_event_routes, bb_event_routes_register_routes_init);
#endif
