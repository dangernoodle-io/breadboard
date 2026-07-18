// THROWAWAY DE-RISK EXAMPLE (B1-1033 first bench-flash de-risk, design
// KB 1447) -- proves the bb_data_http ESP-IDF backend
// (platform/espidf/bb_data_http/bb_data_http_espidf.c) on real hardware
// BEFORE the B1-1045 atomic egress cutover: ONE broadcaster task, an
// httpd-async SSE connect handler, and the socket-hardening/peer-liveness
// pre-pass all get their first real-hardware exercise here.
//
// FENCE: this whole example directory is DELETED wholesale at the
// B1-1045 cutover, not evolved into anything -- do not add new routes,
// bindings, or generality here. It exists only to bench-flash-validate the
// espidf backend on a real board: a single TEST binding (test.gauge, STATE
// replay, touched ~1s by a bb_timer job) behind a TEMP GET /api/test-stream
// SSE route. Hand-wired app_main (no codegen, no bb_init walker) -- not the
// smoke matrix, not the floor heap baseline.
#include "bb_log.h"
#include "bb_lifecycle.h"
#include "bb_http_server.h"
#include "bb_storage_nvs.h"
#include "bb_wifi.h"
#include "bb_data.h"
#include "bb_data_http.h"
#include "bb_meminfo.h"
#include "bb_serialize_json.h"
#include "bb_timer.h"

#include <stddef.h>

static const char *TAG = "derisk_app";

// TEST binding touch interval (KB 1447: "~1s").
#define DERISK_GAUGE_TICK_MS 1000

// bb_lifecycle handle for the "http" service -- mirrors examples/floor's
// own httpd-via-bb_lifecycle wiring.
static bb_lifecycle_svc_t s_http_svc = BB_LIFECYCLE_SVC_INVALID;

// ---------------------------------------------------------------------------
// TEST binding: test.gauge {counter, heap_free} -- STATE replay (default),
// touched ~1s by a bb_timer job. Both fields widened to uint64_t (mirrors
// bb_meminfo_heap_snap_t's own widening rationale -- bb_serialize_walk's
// BB_TYPE_U64 case always memcpy()s a fixed 8 bytes at the descriptor
// offset).
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t counter;
    uint64_t heap_free;
} derisk_gauge_t;

static const bb_serialize_field_t s_gauge_fields[] = {
    { .key = "counter", .type = BB_TYPE_U64, .offset = offsetof(derisk_gauge_t, counter) },
    { .key = "heap_free", .type = BB_TYPE_U64, .offset = offsetof(derisk_gauge_t, heap_free) },
};

static const bb_serialize_desc_t s_gauge_desc = {
    .type_name = "gauge",
    .fields    = s_gauge_fields,
    .n_fields  = sizeof(s_gauge_fields) / sizeof(s_gauge_fields[0]),
    .snap_size = sizeof(derisk_gauge_t),
};

static uint64_t s_counter;

static bb_err_t gather_gauge(void *dst, void *ctx)
{
    (void)ctx;
    derisk_gauge_t *g = (derisk_gauge_t *)dst;
    g->counter = s_counter;

    bb_meminfo_snapshot_t snap;
    g->heap_free = (bb_meminfo_get(&snap) == BB_OK) ? (uint64_t)snap.default_region.free : 0;
    return BB_OK;
}

// bb_timer job: increments the counter and bumps test.gauge's generation --
// the broadcaster's next sweep detects the change, renders, and drains it
// to every connected SSE client.
static void gauge_tick(void *arg)
{
    (void)arg;
    s_counter++;
    bb_err_t err = bb_data_touch("test.gauge");
    if (err != BB_OK) {
        bb_log_w(TAG, "data_touch(test.gauge) failed: %d", (int)err);
    }
}

// ---------------------------------------------------------------------------
// TEMP route: GET /api/test-stream -- handwired straight onto
// bb_data_http_espidf_client_connect(). No topic filter (NULL): this
// example attaches exactly one key, so every client receives it.
// ---------------------------------------------------------------------------
static bb_err_t test_stream_handler(bb_http_request_t *req)
{
    return bb_data_http_espidf_client_connect(req, NULL);
}

// bb_lifecycle observer: starts httpd + the bb_data_http broadcaster +
// registers the TEMP route on the "http" service's RUNNING transition,
// stops httpd on exit -- mirrors examples/floor's http_lifecycle_observer.
static void http_lifecycle_observer(const bb_lifecycle_event_t *evt, void *user)
{
    (void)user;
    if (evt->svc != s_http_svc) {
        return;
    }
    if (evt->new_state == BB_LIFECYCLE_RUNNING && evt->old_state != BB_LIFECYCLE_RUNNING) {
        bb_err_t err = bb_http_server_start();
        if (err != BB_OK) {
            bb_log_w(TAG, "http_server_start failed (%d)", (int)err);
            return;
        }

        // Handwired after httpd RUNNING (KB 1447 fork #3: no core
        // heartbeat/lifecycle wiring for the broadcaster itself in this
        // de-risk -- this TEST binding is never idle, so there is nothing
        // for a heartbeat to cover here).
        err = bb_data_http_espidf_start();
        if (err != BB_OK) {
            bb_log_w(TAG, "data_http_espidf_start failed (%d)", (int)err);
        }

        bb_http_handle_t server = bb_http_server_get_handle();
        // Plain bb_http_register_route(), not the described/OpenAPI
        // variant -- deliberate, mirrors examples/floor: this throwaway
        // example has no bb_openapi dependency and must not gain one.
        bb_err_t route_err = bb_http_register_route(
            server, BB_HTTP_GET, "/api/test-stream", test_stream_handler);
        if (route_err != BB_OK) {
            bb_log_w(TAG, "route /api/test-stream failed (%d)", (int)route_err);
        }
    } else if (evt->old_state == BB_LIFECYCLE_RUNNING && evt->new_state != BB_LIFECYCLE_RUNNING) {
        bb_err_t err = bb_http_server_stop();
        if (err != BB_OK) {
            bb_log_w(TAG, "http_server_stop failed (%d)", (int)err);
        }
    }
}

void app_main(void)
{
    bb_log_stream_init();
    bb_log_config_init();

    bb_log_i(TAG, "boot");

    // Net stack + WiFi (async connect, reads NVS creds) -- same sequencing
    // as examples/floor.
    bb_storage_nvs_register();
    bb_wifi_ensure_net_stack();
    bb_lifecycle_autoinit();
    bb_wifi_autoinit();

    // bb_data egress: init the client-slot table, register the JSON format
    // backend, bind the one TEST key, and attach it under the "test" topic.
    // Composition-time-only calls -- must happen before bb_lifecycle_start()
    // lets any request in. bb_data_http_init() specifically must precede any
    // bb_data_http_client_acquire_ex() call (the connect handler's own
    // acquire otherwise fails BB_ERR_INVALID_STATE on every connection --
    // see bb_data_http.h's bb_data_http_client_acquire_ex() doc).
    bb_err_t err = bb_data_http_init(NULL);
    if (err != BB_OK) {
        bb_log_w(TAG, "data_http_init failed (%d)", (int)err);
    }
    err = bb_serialize_json_register_format();
    if (err != BB_OK) {
        bb_log_w(TAG, "serialize_json_register_format failed (%d)", (int)err);
    }
    err = bb_data_bind(&(bb_data_binding_t){
        .key = "test.gauge", .desc = &s_gauge_desc,
        .gather = gather_gauge, .ctx = NULL });
    if (err != BB_OK) {
        bb_log_w(TAG, "data_bind(test.gauge) failed (%d)", (int)err);
    }
    err = bb_data_http_attach("test.gauge", "test");
    if (err != BB_OK) {
        bb_log_w(TAG, "data_http_attach(test.gauge) failed (%d)", (int)err);
    }

    // Register + start the "http" service -- the observer fires
    // synchronously from bb_lifecycle_start(), so httpd + the broadcaster +
    // the route are all up by the time this call returns.
    bb_lifecycle_config_t http_cfg = { .name = "http" };
    err = bb_lifecycle_register(&http_cfg, &s_http_svc);
    if (err != BB_OK) {
        bb_log_w(TAG, "lifecycle_register(http) failed (%d)", (int)err);
    } else {
        err = bb_lifecycle_observe(http_lifecycle_observer, NULL);
        if (err != BB_OK) {
            bb_log_w(TAG, "lifecycle_observe(http) failed (%d)", (int)err);
        } else {
            err = bb_lifecycle_start(s_http_svc);
            if (err != BB_OK) {
                bb_log_w(TAG, "lifecycle_start(http) failed (%d)", (int)err);
            }
        }
    }

    bb_periodic_timer_t gauge_timer = NULL;
    err = bb_timer_deferred_periodic_create(gauge_tick, NULL, "gauge_tick", &gauge_timer);
    if (err == BB_OK) {
        bb_timer_periodic_start(gauge_timer, (uint64_t)DERISK_GAUGE_TICK_MS * 1000ULL);
    } else {
        bb_log_w(TAG, "gauge_tick: timer create failed (%d)", (int)err);
    }

    // app_main returns; the broadcaster runs on its own task, gauge_tick on
    // the shared bb_timer_disp task.
}
