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

    // Register + wire the "http" service, then start it -- the observer
    // fires synchronously from bb_lifecycle_start(), so httpd is up by the
    // time this call returns.
    bb_lifecycle_config_t http_cfg = { .name = "http" };
    bb_err_t err = bb_lifecycle_register(&http_cfg, &s_http_svc);
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
