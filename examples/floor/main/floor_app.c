// FOUNDATIONAL FLOOR (jae/floor-handwire, decision #724): the true minimal
// bootable base -- bb_log + while(1) print, HAND-WIRED (a la carte). Self-
// registration is dead; the floor calls each component's init directly and
// does not depend on the bb_init runtime walker at boot. Every layered-up bb
// example should be measurably larger than this floor.
//
// B1-973 slice 1a: bb_lifecycle now manages the HTTP server's start/stop --
// its first real consumer -- so floor can take a heap-with-HTTP reading
// alongside the existing HTTP-less baseline. WiFi/NVS/settings are composed
// only to get the server up on a real board (bb_wifi_autoinit requires=
// storage_nvs).
//
// B1-1045 PR-4 (cutover): floor expands into the full Model-B egress vehicle
// -- the "wifi" bb_lifecycle service is started and repointed onto bb_wifi's
// emit seam (dissolving the dormant PR-1 scaffolding), and bb_data_http is
// the converged SSE/WS broadcaster served at /api/events. Floor binds only
// the "log" producer key from the dissolved bb_event family (the other 5 --
// ota.progress/health.stack/diag.boot/health.display/update.available --
// stay out of floor's scope; their bb_data_touch()-based production is
// unaffected, they're just not bound here). Floor's heap-baseline identity
// moves as a result -- accepted, near-term measurement vehicle only;
// smoke's own event-cutover rehab is B1-1051.
#include "bb_log.h"
#include "bb_serialize_console.h"
#include "bb_timer.h"
#include "bb_lifecycle.h"
#include "bb_http_server.h"
#include "bb_storage_nvs.h"
#include "bb_wifi.h"
#include "bb_data.h"
#include "bb_data_http.h"
#include "bb_meminfo_heap_snap.h"
#include "bb_system_snap.h"
#include "bb_serialize_json.h"
#include "bb_log_event.h"
#include "bb_log_event_wire.h"
#include "bb_mqtt_client.h"
#include "bb_mdns.h"
#include <inttypes.h>
#include <stdbool.h>

static const char *TAG = "floor_app";

// Heap baseline interval (ms). One-off Kconfig knob would be overkill for
// the floor's single hand-wired job; a #define matches the floor's
// hand-wired-not-registry-driven ethos.
#define FLOOR_HEAP_LOG_INTERVAL_MS 5000

// bb_lifecycle handle for the "http" service. Init to the invalid sentinel
// so an unexpected pre-register observer fire is a no-op, never a stale
// zero-handle match.
static bb_lifecycle_svc_t s_http_svc = BB_LIFECYCLE_SVC_INVALID;

// B1-1045 PR-4 cutover: the "wifi" bb_lifecycle service + its caller-owned
// emit-seam binding (bb_lifecycle_binding_t, bb_lifecycle_emit_binding_init)
// are now STARTED and repointed onto bb_wifi's emit seam (bb_wifi_set_emit)
// -- the PR-1 scaffolding was registered-but-dormant; this is the real
// cutover. See the INIT-ORDER HAZARD comment on the wiring block in
// app_main() below.
static bb_lifecycle_svc_t     s_wifi_svc = BB_LIFECYCLE_SVC_INVALID;
static bb_lifecycle_binding_t s_wifi_lifecycle_binding;

// Real httpd socket state, set synchronously by http_lifecycle_observer (it
// fires inside bb_lifecycle_start() on this same task) -- the SSOT for
// whether bb_http_server_start() actually succeeded. bb_lifecycle_start()
// itself only reports the state-machine transition, not the observer's
// result (notify_all() is void), so this flag -- not the lifecycle
// register/observe/start return codes -- gates the with-HTTP reading.
static bool s_httpd_up = false;

// The floor's first telemetry SOURCE (heap): emitted via
// bb_serialize_console_heap_report() -- gather (bb_meminfo) -> render
// (bb_serialize_walk over bb_serialize_console_heap_desc) -> bb_log_i(),
// all internal to that call. Measured, not published -- serial only, not
// yet wired to a telemetry sink. Runs as a bb_timer MODE-A job on the
// shared bb_timer_disp task -- no dedicated task. floor is the first live
// app call site of the serialization emit stack.
static void heap_log_tick(void *arg)
{
    (void)arg;
    bb_serialize_console_heap_report("tick");
}

// bb_data egress buffer (JSON render) -- sized to provably exceed the
// theoretical worst-case bb_serialize_json_bound() of any bound descriptor,
// not a guessed constant. bb_serialize_json_bound() computed against this
// branch's descriptors (11 uint64 leaves in bb_meminfo_heap_snap_desc, the
// largest -- 20-digit-max per-integer worst case, 6x-per-char key/string
// escape-expansion bound, plus structural punctuation) returns 2319 bytes;
// every producer descriptor stays well under that. Every /api/diag/* route
// and the /api/events broadcaster share this one constant, each call/sweep
// owns its own stack buffer (no static/shared scratch, reentrant).
// Rounded up generously over the 2319 worst case for headroom against a
// future field addition.
#define FLOOR_DIAG_RENDER_BUF_SIZE 2560

// bb_data gather adapters: bb_data_gather_fn's signature is untyped
// (void *dst, const bb_data_gather_args_t *args); each snap's own
// _fill()/_gather() fn takes its typed pointer, so these thin shims are the
// only cast needed. None of these three keys take request-scoped query
// params yet -- args->query is ignored.
static bb_err_t gather_meminfo(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    return bb_meminfo_heap_snap_fill((bb_meminfo_heap_snap_t *)dst);
}

static bb_err_t gather_system(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    return bb_system_snap_fill((bb_system_snap_t *)dst);
}

// B1-1045 PR-4: gather adapter wrapping the dissolved-bb_event "log"
// producer's PR-2 plain gather -- portable/host-testable at the producer's
// own layer (see bb_log_event_gather()'s doc comment); this composition
// root just supplies the untyped bb_data_gather_fn cast.
static bb_err_t gather_log(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    return bb_log_event_gather((bb_log_event_wire_t *)dst);
}

// Common render+respond body for a /api/diag/* handler: renders `key` via
// bb_data_render() into the caller's `scratch`/`buf`, then finishes the HTTP
// response. Content-Type is set on BOTH the success and the 500 paths (the
// 500 branch's JSON error body needs it too -- bb_http_resp_sendstr() itself
// does not set Content-Type).
static bb_err_t floor_diag_render(bb_http_request_t *req, const char *key,
                                   void *scratch, size_t scratch_cap)
{
    char   buf[FLOOR_DIAG_RENDER_BUF_SIZE];
    size_t len = 0;

    bb_data_render_req_t render_req = {
        .fmt = BB_FORMAT_JSON, .key = key, .query = NULL,
        .scratch = scratch, .scratch_cap = scratch_cap,
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &len,
    };
    bb_err_t rc = bb_data_render(&render_req);
    bb_http_resp_set_type(req, "application/json");
    if (rc != BB_OK) {
        bb_http_resp_set_status(req, 500);
        return bb_http_resp_sendstr(req, "{\"error\":\"render failed\"}");
    }
    return bb_http_resp_sendstr(req, buf);
}

// GET /api/diag/meminfo -- fresh render every request (no cache): gather ->
// bb_data_render() -> JSON bytes, via the shared floor_diag_render() helper.
static bb_err_t meminfo_get_handler(bb_http_request_t *req)
{
    bb_meminfo_heap_snap_t scratch;
    return floor_diag_render(req, "diag.meminfo", &scratch, sizeof(scratch));
}

// GET /api/diag/system -- same shape as meminfo_get_handler, over the
// bb_system snapshot.
static bb_err_t system_get_handler(bb_http_request_t *req)
{
    bb_system_snap_t scratch;
    return floor_diag_render(req, "diag.system", &scratch, sizeof(scratch));
}

// GET /api/events?topic=<key> -- the bb_data_http broadcaster route (B1-1045
// repoint of the old bb_event_routes-served /api/events). Generalizes the
// bb_data_http_derisk example's single-key test_stream_handler with real
// topic-param parsing (mirrors the retired bb_event_routes_espidf.c
// events_handler's "topic" query-param extraction): omit the param to
// receive every attached key, or name one to filter the stream to it.
static bb_err_t events_get_handler(bb_http_request_t *req)
{
    char topic_buf[BB_DATA_HTTP_TOPIC_MAX] = {0};
    const char *topic_filter = NULL;
    if (bb_http_req_query_key_value(req, "topic", topic_buf, sizeof(topic_buf)) == BB_OK) {
        topic_filter = topic_buf;
    }
    return bb_data_http_espidf_client_connect(req, topic_filter);
}

// bb_lifecycle observer: bb_http_server_start()/stop() are driven purely by
// the "http" service's RUNNING transitions -- start on entry to RUNNING,
// stop on exit from RUNNING. Fires synchronously, outside bb_lifecycle's
// lock; MUST NOT call any bb_lifecycle mutator from here (start/stop/
// register/pause_*) -- bb_http_server_start/stop are not lifecycle
// mutators, so they're safe.
static void http_lifecycle_observer(const bb_lifecycle_event_t *evt, void *user)
{
    (void)user;
    if (evt->svc != s_http_svc) {
        return;
    }
    if (evt->new_state == BB_LIFECYCLE_RUNNING && evt->old_state != BB_LIFECYCLE_RUNNING) {
        bb_err_t err = bb_http_server_start();
        s_httpd_up = (err == BB_OK);
        if (err != BB_OK) {
            bb_log_w(TAG, "http_server_start failed (%d)", (int)err);
        } else {
            bb_http_handle_t server = bb_http_server_get_handle();
            // Plain bb_http_register_route(), not the described/OpenAPI
            // variant -- deliberate: floor has no bb_openapi dependency and
            // must not gain one (it's the minimal measurement rig), so these
            // routes are intentionally absent from the OpenAPI spec walk.
            bb_err_t route_err = bb_http_register_route(
                server, BB_HTTP_GET, "/api/diag/meminfo", meminfo_get_handler);
            if (route_err != BB_OK) {
                bb_log_w(TAG, "route /api/diag/meminfo failed (%d)", (int)route_err);
            }
            route_err = bb_http_register_route(
                server, BB_HTTP_GET, "/api/diag/system", system_get_handler);
            if (route_err != BB_OK) {
                bb_log_w(TAG, "route /api/diag/system failed (%d)", (int)route_err);
            }
            // B1-1045: start the bb_data_http broadcaster task (idempotent)
            // and serve /api/events off it now that the server is up.
            bb_err_t bcast_err = bb_data_http_espidf_start();
            if (bcast_err != BB_OK) {
                bb_log_w(TAG, "data_http_espidf_start failed (%d)", (int)bcast_err);
            }
            route_err = bb_http_register_route(
                server, BB_HTTP_GET, "/api/events", events_get_handler);
            if (route_err != BB_OK) {
                bb_log_w(TAG, "route /api/events failed (%d)", (int)route_err);
            }
        }
    } else if (evt->old_state == BB_LIFECYCLE_RUNNING && evt->new_state != BB_LIFECYCLE_RUNNING) {
        bb_err_t err = bb_http_server_stop();
        if (err != BB_OK) {
            bb_log_w(TAG, "http_server_stop failed (%d)", (int)err);
        }
        s_httpd_up = false;
    }
}

void app_main(void)
{
    // Hand-wired in the same order the bb_init EARLY tier previously
    // replayed them via BB_INIT_REGISTER_EARLY (constructor/link order,
    // not the walker): the log stream worker (console writer task + ring
    // buffer) first, then the Kconfig-driven default/per-tag log levels.
    bb_log_stream_init();
    bb_log_config_init();

    bb_log_i(TAG, "boot");

    // Net stack + storage_nvs (creds) -- composed only so the HTTP server
    // has a real board to run on for the heap-with-HTTP reading; bb_wifi_
    // autoinit requires=storage_nvs, so bb_storage_nvs registers first.
    // bb_wifi_autoinit() itself is deferred past the "wifi" lifecycle wiring
    // block below (INIT-ORDER HAZARD) -- it must not run until the emit seam
    // is repointed, or the first GOT_IP is dropped.
    bb_storage_nvs_register();
    bb_wifi_ensure_net_stack();
    bb_lifecycle_autoinit();

    // Baseline reading: HTTP stopped, WiFi not yet started.
    bb_serialize_console_heap_report("baseline-http-stopped");

    // ---------------------------------------------------------------------
    // B1-1045 PR-4 cutover -- INIT-ORDER HAZARD: register + START the
    // "wifi" bb_lifecycle service and repoint bb_wifi's emit seam onto it
    // BEFORE bb_wifi_autoinit() runs below. bb_wifi_autoinit() can fire its
    // first GOT_IP synchronously from the connect path; if the trampoline
    // binding/emit seam is not wired yet, that first edge is silently
    // dropped (no retry -- bb_wifi has no event replay). The "wifi" service
    // is registered here (this PR starts what PR-1 left dormant).
    // ---------------------------------------------------------------------
    bb_lifecycle_config_t wifi_cfg = { .name = "wifi" };
    bb_err_t err = bb_lifecycle_register(&wifi_cfg, &s_wifi_svc);
    if (err != BB_OK) {
        bb_log_w(TAG, "lifecycle_register(wifi) failed (%d)", (int)err);
    } else {
        err = bb_lifecycle_emit_binding_init(&s_wifi_lifecycle_binding, s_wifi_svc,
                                             bb_wifi_classify_lifecycle);
        if (err != BB_OK) {
            bb_log_w(TAG, "lifecycle_emit_binding_init(wifi) failed (%d)", (int)err);
        } else {
            bb_wifi_set_emit(bb_lifecycle_emit_binding_fn(), &s_wifi_lifecycle_binding);
        }
        err = bb_lifecycle_start(s_wifi_svc);
        if (err != BB_OK) {
            bb_log_w(TAG, "lifecycle_start(wifi) failed (%d)", (int)err);
        }
    }

    // bb_data_http core init -- BEFORE any client_connect (the /api/events
    // route registered below, on the "http" service's start edge).
    err = bb_data_http_init(NULL);
    if (err != BB_OK) {
        bb_log_w(TAG, "data_http_init failed (%d)", (int)err);
    }

    // bb_data egress: register the JSON format backend, then bind every
    // key the routes/broadcaster render on demand. Composition-time-only
    // calls -- must happen before bb_lifecycle_start(http) lets any request
    // in and before the broadcaster's first sweep.
    err = bb_serialize_json_register_format();
    if (err != BB_OK) {
        bb_log_w(TAG, "serialize_json_register_format failed (%d)", (int)err);
    }
    err = bb_data_bind(&(bb_data_binding_t){
        .key = "diag.meminfo", .desc = &bb_meminfo_heap_snap_desc,
        .gather = gather_meminfo, .ctx = NULL });
    if (err != BB_OK) {
        bb_log_w(TAG, "data_bind(diag.meminfo) failed (%d)", (int)err);
    }
    err = bb_data_bind(&(bb_data_binding_t){
        .key = "diag.system", .desc = &bb_system_snap_desc,
        .gather = gather_system, .ctx = NULL });
    if (err != BB_OK) {
        bb_log_w(TAG, "data_bind(diag.system) failed (%d)", (int)err);
    }

    // B1-1045 PR-4: bind the "log" dissolved-bb_event producer key. Its
    // underlying stash is populated by bb_log_event's own forwarder task
    // (started below, once the http server is up) -- this proves the
    // bb_data bind/render/broadcast closure compiles, links, and serves
    // end to end. The other 5 dissolved-bb_event producer keys stay out of
    // floor's scope (see the file-header comment); smoke's own
    // event-cutover rehab is B1-1051.
    static const struct {
        const char                *key;
        const bb_serialize_desc_t *desc;
        bb_data_gather_fn          gather;
    } producers[] = {
        { "log", &bb_log_event_wire_desc, gather_log },
    };
    for (size_t i = 0; i < sizeof(producers) / sizeof(producers[0]); i++) {
        err = bb_data_bind(&(bb_data_binding_t){
            .key = producers[i].key, .desc = producers[i].desc,
            .gather = producers[i].gather, .ctx = NULL });
        if (err != BB_OK) {
            bb_log_w(TAG, "data_bind(%s) failed (%d)", producers[i].key, (int)err);
        }
        // _sized: catches a producer wire type that would exceed the shared
        // render scratch (CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES) as a
        // loud attach-time failure instead of a silently-starved stream
        // (B1-1045 PR-4 fix -- see bb_data_http_attach_sized()'s doc).
        err = bb_data_http_attach_sized(producers[i].key, producers[i].key,
                                        BB_DATA_HTTP_STATE, producers[i].desc->snap_size);
        if (err != BB_OK) {
            bb_log_w(TAG, "data_http_attach(%s) failed (%d)", producers[i].key, (int)err);
        }
    }

    // Composition-time guard: bb_serialize_json_bound() is the true
    // worst-case byte count for rendering each bound descriptor as JSON --
    // verify both /api/diag/* descriptors stay within FLOOR_DIAG_RENDER_BUF_SIZE
    // here, once, rather than discovering a future field addition via a
    // silent BB_ERR_NO_SPACE truncation on real hardware. Not a hard assert
    // (a device-side over-bound is a code defect, not a runtime condition
    // worth crashing boot over) -- log an error loudly enough to be
    // unmissable in CI/bench boot output.
    size_t meminfo_bound = bb_serialize_json_bound(&bb_meminfo_heap_snap_desc);
    if (meminfo_bound > FLOOR_DIAG_RENDER_BUF_SIZE) {
        bb_log_e(TAG, "diag.meminfo worst-case JSON (%zu) exceeds FLOOR_DIAG_RENDER_BUF_SIZE (%d)",
                 meminfo_bound, FLOOR_DIAG_RENDER_BUF_SIZE);
    }
    size_t system_bound = bb_serialize_json_bound(&bb_system_snap_desc);
    if (system_bound > FLOOR_DIAG_RENDER_BUF_SIZE) {
        bb_log_e(TAG, "diag.system worst-case JSON (%zu) exceeds FLOOR_DIAG_RENDER_BUF_SIZE (%d)",
                 system_bound, FLOOR_DIAG_RENDER_BUF_SIZE);
    }

    // Now safe: the "wifi" lifecycle service is registered/started and
    // bb_wifi's emit seam is repointed onto it (see the INIT-ORDER HAZARD
    // block above) -- the first GOT_IP this drives will be observed.
    bb_wifi_autoinit();

    // Observe the "wifi" service from the mqtt/mdns consumers (B1-1045
    // consumer conversions, PR-4): each component registers its own
    // bb_lifecycle_observe_async() call internally, guarded to attach at
    // most once, the first time its own init runs -- see
    // platform/espidf/bb_mqtt_client/bb_mqtt_client_espidf.c and
    // platform/espidf/bb_mdns/bb_mdns.c.
    err = bb_mqtt_client_init_default();
    if (err != BB_OK) {
        bb_log_w(TAG, "mqtt_client_init_default failed (%d)", (int)err);
    }
    bb_mdns_init();

    // Register + wire the "http" service, then start it -- the observer
    // fires synchronously from bb_lifecycle_start(), so httpd is up by the
    // time this call returns.
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
            // bb_lifecycle_start() only reports the state-machine
            // transition -- it never propagates the observer's result
            // (notify_all() is void) -- so BB_OK here does NOT mean httpd
            // is actually up. If register/observe/start fail, the
            // observer's start edge never fires and s_httpd_up stays
            // false, so no separate else-branch is needed here.
        }
    }

    // Delta reading: HTTP running. Gate on the REAL httpd socket state
    // (s_httpd_up, set synchronously by http_lifecycle_observer's start
    // edge), not on the lifecycle register/observe/start return codes --
    // those only confirm the state machine transitioned, not that
    // bb_http_server_start() inside the observer actually succeeded.
    if (s_httpd_up) {
        bb_serialize_console_heap_report("with-http-running");

        // B1-1045 PR-4: bring up the "log" producer's forwarder queue+task
        // now that the server is up and the "log" bb_data key is bound
        // (see the producers[] bind loop above) -- bb_log_event_init()
        // hooks the log vprintf via bb_log_event_set_queue() so
        // s_forwarder_task starts stashing + bb_data_touch("log")ing on
        // every log line. The server handle param is currently unused by
        // bb_log_event_init() but passed for API-shape parity with the
        // codegen call site (examples/floor/main/generated/bb_app_init.c).
        err = bb_log_event_init(bb_http_server_get_handle());
        if (err != BB_OK) {
            bb_log_w(TAG, "log_event_init failed (%d)", (int)err);
        }
    } else {
        bb_log_w(TAG, "http not started -- with-http reading skipped");
    }

    bb_periodic_timer_t heap_log_timer = NULL;
    err = bb_timer_deferred_periodic_create(
        heap_log_tick, NULL, "heap_log", &heap_log_timer);
    if (err == BB_OK) {
        bb_timer_periodic_start(heap_log_timer,
                                (uint64_t)FLOOR_HEAP_LOG_INTERVAL_MS * 1000ULL);
    } else {
        bb_log_w(TAG, "heap_log: timer create failed (%d)", (int)err);
    }

    // app_main returns; heap-log job runs on the bb_timer_disp task.
}
