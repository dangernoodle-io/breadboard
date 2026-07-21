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
#include "bb_system_snap.h"
#include "bb_diag_section.h"
#include "bb_diag_meminfo.h"
#include "bb_diag_storage_nvs.h"
#include "bb_diag_storage_partitions.h"
#include "bb_serialize_json.h"
#include "bb_log_event.h"
#include "bb_log_event_wire.h"
#include "bb_mqtt_client.h"
#include "bb_mdns.h"
#include "bb_health.h"
#include "bb_temp.h"
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

// bb_diag section fill adapter: bb_diag_fill_fn's signature is untyped
// (void *dst, const bb_diag_fill_args_t *args); bb_system_snap_fill()'s
// typed pointer needs this thin shim as the only cast. "meminfo"'s adapter
// now lives in bb_diag_meminfo.c (heap reconciliation, B1-diag-dissolution
// -- promoted to a shared section file so smoke's codegen gets it too).
// Neither "meminfo" nor "system" declares query_keys -- args->query is
// always NULL for these two sections.
static bb_err_t diag_fill_system(void *dst, const bb_diag_fill_args_t *args)
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
            // PR11 (B1-diag-dissolution): register the GET /api/diag/*
            // wildcard dispatcher -- this DOES need the server handle, so it
            // stays here on the RUNNING-entry edge. The four sections it
            // serves are registered once, composition-time, in app_main()
            // (they write to bb_diag's global section table, no server
            // handle needed, and this observer branch re-fires on every
            // pause/resume, which would otherwise reject them as dupes).
            bb_err_t route_err = bb_diag_sections_init(server);
            if (route_err != BB_OK) {
                bb_log_w(TAG, "diag_sections_init failed (%d)", (int)route_err);
            }
            // B1-1100 cutover: GET /api/health, gather-then-stream composed
            // from the bb_health_section registry (mqtt/temp registered
            // below, composition-time-only, before the server ever starts).
            // Freezes the registry (the FREEZE TRIPWIRE) as a side effect.
            route_err = bb_health_init(server);
            if (route_err != BB_OK) {
                bb_log_w(TAG, "health_init failed (%d)", (int)route_err);
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

    // bb_diag sections: composition-time-only, once -- these write to
    // bb_diag's global section table (reject-on-duplicate) and need no
    // server handle, so they belong here rather than in the "http" service's
    // RUNNING-entry observer branch (which re-fires on every pause/resume).
    // The /api/diag/* route dispatch itself (bb_diag_sections_init(), which
    // DOES need the server handle) stays in http_lifecycle_observer.
    err = bb_diag_meminfo_register();
    if (err != BB_OK) {
        bb_log_w(TAG, "diag_meminfo_register failed (%d)", (int)err);
    }
    err = bb_diag_register_section(&(bb_diag_section_t){
        .name = "system", .desc = "system info snapshot",
        .snap_desc = &bb_system_snap_desc, .fill = diag_fill_system });
    if (err != BB_OK) {
        bb_log_w(TAG, "diag_register_section(system) failed (%d)", (int)err);
    }
    err = bb_diag_storage_nvs_register();
    if (err != BB_OK) {
        bb_log_w(TAG, "diag_storage_nvs_register failed (%d)", (int)err);
    }
    err = bb_diag_storage_partitions_register();
    if (err != BB_OK) {
        bb_log_w(TAG, "diag_storage_partitions_register failed (%d)", (int)err);
    }

    // B1-1100: /api/health sections -- composition-time-only, before the
    // bb_health_section registry is frozen (bb_health_init(), called from
    // http_lifecycle_observer's RUNNING-entry branch below). "temp" is a
    // fill-agnostic satellite (bb_temp_autoregister_init() is its
    // // bbtool:init marker fn, called directly here per floor's handwire
    // convention); "mqtt" reads bb_mqtt_client_default() at request time,
    // so it's safe to register before bb_mqtt_client_init_default() runs.
    err = bb_temp_autoregister_init();
    if (err != BB_OK) {
        bb_log_w(TAG, "temp_autoregister_init failed (%d)", (int)err);
    }
    err = bb_mqtt_client_health_autoregister_init();
    if (err != BB_OK) {
        bb_log_w(TAG, "mqtt_client_health_autoregister_init failed (%d)", (int)err);
    }
    // bb_health_reserve_routes() (PRE_HTTP tier) is intentionally not called
    // here -- it is vestigial (platform/espidf/bb_http_server/bb_http.c: max_
    // uri_handlers is a single constant knob now, route count no longer
    // drives it), matching floor's existing selectivity (it calls no other
    // component's *_reserve_routes() either).

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
