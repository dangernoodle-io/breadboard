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
// storage_nvs); no route table, no /api surface -- an empty httpd is enough
// to measure the server's own heap footprint.
#include "bb_log.h"
#include "bb_serialize_console.h"
#include "bb_timer.h"
#include "bb_lifecycle.h"
#include "bb_http_server.h"
#include "bb_storage_nvs.h"
#include "bb_wifi.h"
#include "bb_data.h"
#include "bb_meminfo_heap_snap.h"
#include "bb_system_snap.h"
#include "bb_serialize_json.h"
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
// theoretical worst-case bb_serialize_json_bound() of either snapshot
// descriptor, not a guessed constant. bb_serialize_json_bound() computed
// against this branch's descriptors (11 uint64 leaves in
// bb_meminfo_heap_snap_desc, the larger of the two -- 20-digit-max
// per-integer worst case, 6x-per-char key/string escape-expansion bound,
// plus structural punctuation) returns 2319 bytes; bb_system_snap_desc
// returns 1359. Both diag routes share this one constant, each call owns
// its own stack buffer (no static/shared scratch, reentrant per request).
// Rounded up generously over the 2319 worst case for headroom against a
// future field addition.
#define FLOOR_DIAG_RENDER_BUF_SIZE 2560

// bb_data gather adapters: bb_data_gather_fn's signature is untyped
// (void *dst, void *ctx); each snap's own _fill() fn takes its typed
// pointer, so these thin shims are the only cast needed.
static bb_err_t gather_meminfo(void *dst, void *ctx)
{
    (void)ctx;
    return bb_meminfo_heap_snap_fill((bb_meminfo_heap_snap_t *)dst);
}

static bb_err_t gather_system(void *dst, void *ctx)
{
    (void)ctx;
    return bb_system_snap_fill((bb_system_snap_t *)dst);
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

    bb_err_t rc = bb_data_render(BB_FORMAT_JSON, key, scratch, scratch_cap, buf, sizeof(buf), &len);
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

    // Net stack + WiFi (async connect, reads NVS creds) -- composed only so
    // the HTTP server has a real board to run on for the heap-with-HTTP
    // reading; bb_wifi_autoinit requires=storage_nvs, so bb_storage_nvs
    // registers first.
    bb_storage_nvs_register();
    bb_wifi_ensure_net_stack();
    bb_lifecycle_autoinit();
    bb_wifi_autoinit();

    // Baseline reading: HTTP stopped.
    bb_serialize_console_heap_report("baseline-http-stopped");

    // bb_data egress: register the JSON format backend, then bind the two
    // diag keys the routes above render on demand. Composition-time-only
    // calls -- must happen before bb_lifecycle_start() lets any request in.
    bb_err_t err = bb_serialize_json_register_format();
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

    // Composition-time guard: bb_serialize_json_bound() is the true
    // worst-case byte count for rendering each bound descriptor as JSON --
    // verify both stay within FLOOR_DIAG_RENDER_BUF_SIZE here, once, rather
    // than discovering a future field addition via a silent BB_ERR_NO_SPACE
    // truncation on real hardware. Not a hard assert (a device-side
    // over-bound is a code defect, not a runtime condition worth crashing
    // boot over) -- log an error loudly enough to be unmissable in CI/bench
    // boot output.
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
