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
#include "bb_settings.h"
#include "bb_wifi.h"
#include "bb_data.h"
#include "bb_data_http.h"
#include "bb_system_snap.h"
#include "bb_diag_section.h"
#include "bb_diag_http.h"
#include "bb_storage_http.h"
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
#include "bb_wifi_prov.h"
#include "bb_wifi_prov_default_form.h"
#include "bb_task_registry.h"
#include "bb_system.h"
#include "floor_prov_reboot.h"
#include "floor_task_stack.h"
#include <inttypes.h>
#include <stdbool.h>

static const char *TAG = "floor_app";

// Heap baseline interval (ms). One-off Kconfig knob would be overkill for
// the floor's single hand-wired job; a #define matches the floor's
// hand-wired-not-registry-driven ethos.
#define FLOOR_HEAP_LOG_INTERVAL_MS 5000

// Task-stack report interval (ms). Deliberately its OWN timer, not reused
// off heap_log_tick: the data this reports (bb_task_base_foreach()'s
// free_bytes) only changes once per periodic base-scan pass
// (BB_TASK_REGISTRY_BASE_POLL_MS, Kconfig-bridged default 10s) -- ticking
// at heap's faster 5s cadence would just log the SAME scan's numbers twice
// as often, adding lines without adding information. This interval is a
// touch above that default so each report reflects a scan that has already
// run, not a stale one from before floor's own boot-time reads.
#define FLOOR_TASK_STACK_LOG_INTERVAL_MS 12000

// bb_lifecycle handle for the "http" service. Init to the invalid sentinel
// so an unexpected pre-register observer fire is a no-op, never a stale
// zero-handle match.
static bb_lifecycle_svc_t s_http_svc = BB_LIFECYCLE_SVC_INVALID;

// B1-1045 PR-4 cutover: the "wifi" bb_lifecycle service + its caller-owned
// emit-seam binding (bb_lifecycle_binding_t, bb_lifecycle_emit_binding_init)
// are registered and repointed onto bb_wifi's emit seam (bb_wifi_set_emit)
// -- the PR-1 scaffolding was registered-but-dormant; this is the real
// cutover. The service itself is left STOPPED at composition time and only
// reaches RUNNING via the real GOT_IP edge (see the INIT-ORDER HAZARD
// comment on the wiring block in app_main() below for why an eager start
// there was wrong).
static bb_lifecycle_svc_t     s_wifi_svc = BB_LIFECYCLE_SVC_INVALID;
static bb_lifecycle_binding_t s_wifi_lifecycle_binding;

// Provisioning-is-transient (see the reboot-into-normal-boot block below):
// snapshotted once, before the "wifi" service is wired up and before
// bb_wifi_autoinit() can drive its first GOT_IP, from
// bb_settings_wifi_has_creds() -- the same read bb_wifi_prov_autoinit() does
// internally. Reading it this early is equivalent to reading it right
// before wifi_prov_autoinit() below: nothing between here and there writes
// wifi creds (only a portal /save POST does, and that cannot happen this
// early in boot). Composition-root state, not bb_wifi_prov's -- floor is
// the one deciding to suppress mdns/mqtt and to reboot, per CLAUDE.md's
// composition-only model.
static bool s_prov_mode_boot = false;

// Set once the provisioning-mode reboot has been triggered, so a spurious
// re-fire of the "wifi" service's RUNNING edge (e.g. a flaky
// disconnect/reconnect before esp_restart() actually halts the CPU) cannot
// queue a second bb_system_restart_reason() call.
static bool s_prov_reboot_triggered = false;

// HW-CONFIRMED fix: bb_system_restart_reason()'s NVS reboot-record write and
// esp_restart() must not run inline on the "wifi" service's emit trampoline
// -- that trampoline fires synchronously on the shared ESP-IDF default event
// loop ("sys_evt") task, and a blocking flash write there raced esp_restart()
// closely enough that both the reboot record and the confirming log line
// were lost (HW: preceding log line truncated mid-word, no restart_reason
// log, no NVS record). Mirrors bb_wifi.c's existing bb_wifi_reconfigure()
// reboot: bb_system_restart_deferred (components/bb_system) moves the work
// onto the shared bb_timer_disp task instead -- see wifi_lifecycle_observer
// below.

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

// The floor's second telemetry SOURCE (per-task stack high-water marks):
// migrated (B1-1418 PR-2) off bb_serialize_console's own retired
// bb_serialize_console_tasks_report() onto bb_data's rows-only table-
// producer shape -- floor is the first REAL exercise of
// bb_data_render_rows() outside host tests. The "tasks" key is bound
// rows-only (desc/gather/apply all NULL, rows non-NULL -- see the
// bb_data_bind() call in app_main() below) straight onto
// bb_serialize_console_tasks_row_desc/bb_serialize_console_tasks_gather(),
// the same SSOT primitives the retired entry point used internally
// (bb_task_base_foreach(), populated by bb_task_registry's periodic base-
// scan). Serial only, same measured-not-published posture as
// heap_log_tick above. Own bb_timer MODE-A job (see
// FLOOR_TASK_STACK_LOG_INTERVAL_MS above for why not heap's timer) on the
// shared bb_timer_disp task -- no dedicated task.

// bb_data_row_gather_fn thunk: bb_serialize_console_tasks_gather()'s own
// (dst, cap) shape already matches this signature modulo the unused `args`
// parameter (this row producer takes no query filter), so this is a
// one-line forwarder, not a bespoke gather implementation -- the composition
// root supplies only the untyped bb_data_row_gather_fn cast, matching the
// producers[] table's bb_data_gather_plain thunk precedent above.
static size_t tasks_row_gather(void *dst_rows, size_t max_rows, const bb_data_gather_args_t *args)
{
    (void)args;
    return bb_serialize_console_tasks_gather((bb_serialize_console_tasks_row_snap_t *)dst_rows, max_rows);
}

static const bb_data_row_binding_t s_tasks_row_binding = {
    .row_desc   = &bb_serialize_console_tasks_row_desc,
    .row_gather = tasks_row_gather,
    .row_size   = sizeof(bb_serialize_console_tasks_row_snap_t),
};

// Row-scratch capacity for the "tasks" table producer. Deliberately its OWN
// literal rather than bb_task.h's BB_TASK_BASE_MAX_CAP: floor has no direct
// bb_task dependency today (bb_task is a PRIV_REQUIRES of bb_task_registry/
// bb_serialize_console, not floor's own manifest) and bb_serialize_console.h
// itself documents staying free of a bb_task dependency for the same
// public-header-coupling reason (see BB_SERIALIZE_CONSOLE_TASKS_NAME_MAX's
// own doc comment) -- adding one here solely to size a scratch array would
// be new REQUIRES churn for no behavioral gain, since
// bb_serialize_console_tasks_gather()'s own truncate-don't-overflow contract
// already caps at whichever of the two is smaller. Matches
// BB_TASK_BASE_MAX_CAP's Kconfig default (24) today -- but BB_TASK_BASE_MAX
// (components/bb_task/Kconfig) is a live `range 1 64` knob a board can
// legally raise above this literal, silently truncating the report with no
// signal of its own (firmware review MEDIUM finding, B1-1418 PR-2) -- see
// floor_task_stack.h/task_stack_log_tick() below for the runtime warning
// that makes that drift loud instead of silent.
#define FLOOR_TASK_STACK_ROWS_MAX 24

// File-static rather than a stack local -- same rationale the retired
// bb_serialize_console_tasks_report() itself documented: a stack array sized
// off FLOOR_TASK_STACK_ROWS_MAX would couple this tick's caller's stack
// budget (the shared bb_timer_disp task, BB_TIMER_DISP_STACK) to it.
// Single-caller (task_stack_log_tick is the only bb_data_render_rows() call
// site for this key, a periodic timer job) so a shared static buffer is
// safe -- NOT reentrant.
static bb_serialize_console_tasks_row_snap_t s_task_row_scratch[FLOOR_TASK_STACK_ROWS_MAX];

// bb_data_render_rows_req_t.emit_row: the per-row bb_log_i() call the
// retired bb_serialize_console_tasks_report() used to make internally --
// bb_data stays host-testable and format-neutral (it does NOT call
// bb_log_i() itself), so the composition root supplies this callback
// instead. `ctx` also accumulates `count` -- the number of rows this call
// actually emitted this tick -- so task_stack_log_tick() below can feed it
// to floor_task_stack_should_warn_truncated() (see floor_task_stack.h)
// without a second pass over bb_data_render_rows()'s output; bb_data itself
// exposes no row-count return, only the per-row callback.
typedef struct {
    const char *label;
    size_t      count;
} tasks_row_emit_ctx_t;

static void tasks_row_emit(const char *line, size_t len, size_t row_idx, void *ctx)
{
    (void)len;
    (void)row_idx;
    tasks_row_emit_ctx_t *ec = (tasks_row_emit_ctx_t *)ctx;
    bb_log_i(TAG, "%s task %s", ec->label, line);
    ec->count++;
}

// Latch for the "possibly truncated" warning (firmware review MEDIUM
// finding, B1-1418 PR-2): floor_task_stack_should_warn_truncated() (see
// floor_task_stack.h for the full "why" -- BB_TASK_BASE_MAX is a live
// Kconfig that can outgrow FLOOR_TASK_STACK_ROWS_MAX) decides per-tick
// whether to warn; this static is the one-time latch it consults/this file
// flips, so a registry parked at-or-over capacity warns once per boot, not
// on every 12s tick forever.
static bool s_tasks_truncation_warned = false;

static void task_stack_log_tick(void *arg)
{
    (void)arg;
    tasks_row_emit_ctx_t emit_ctx = { .label = "tick", .count = 0 };
    bb_data_render_rows_req_t req = {
        .key = "tasks", .fmt = BB_FORMAT_CONSOLE,
        .row_scratch = s_task_row_scratch, .max_rows = FLOOR_TASK_STACK_ROWS_MAX,
        .emit_row = tasks_row_emit, .emit_ctx = &emit_ctx,
    };
    bb_err_t err = bb_data_render_rows(&req);
    if (err != BB_OK) {
        bb_log_w(TAG, "data_render_rows(tasks) failed (%d)", (int)err);
        return;
    }
    if (floor_task_stack_should_warn_truncated(emit_ctx.count, FLOOR_TASK_STACK_ROWS_MAX,
                                                s_tasks_truncation_warned)) {
        s_tasks_truncation_warned = true;
        bb_log_w(TAG, "tasks report possibly truncated at %u rows -- "
                 "task registry may exceed floor's row-scratch capacity "
                 "(FLOOR_TASK_STACK_ROWS_MAX)", (unsigned)FLOOR_TASK_STACK_ROWS_MAX);
    }
}

// Shared by both of floor's periodic-log jobs (heap_log_timer,
// task_stack_log_timer): bb_timer_deferred_periodic_create() ->
// bb_timer_periodic_start(), warning and returning NULL on either failure.
// Local to floor (not a bb_timer/bb_core helper) -- this is app-wiring
// boilerplate, not a shared library idiom; smoke's own single instance of
// the same shape wires its own timer independently, per each example's
// hand-wired-not-registry-driven posture.
static bb_periodic_timer_t start_periodic_log_timer(void (*tick)(void *arg),
                                                      const char *name,
                                                      uint64_t interval_ms)
{
    bb_periodic_timer_t timer = NULL;
    bb_err_t err = bb_timer_deferred_periodic_create(tick, NULL, name, &timer);
    if (err != BB_OK) {
        bb_log_w(TAG, "%s: timer create failed (%d)", name, (int)err);
        return NULL;
    }
    bb_timer_periodic_start(timer, interval_ms * 1000ULL);
    return timer;
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

// B1-1045 PR-4 / B1-1415: bb_data_gather_plain()'s ctx wrapping the
// dissolved-bb_event "log" producer's PR-2 plain gather -- portable/
// host-testable at the producer's own layer (see bb_log_event_gather()'s
// doc comment); this composition root just supplies the typed fill pointer,
// the shared thunk supplies the untyped bb_data_gather_fn cast.
static const bb_data_plain_fill_ctx_t s_log_fill_ctx = {
    .fill = (bb_data_plain_fill_fn)bb_log_event_gather,
};

// Provisioning-is-transient reboot trigger: fires on the "wifi" service's
// STOPPED/PAUSED->RUNNING edge, which is exactly the first GOT_IP of this
// boot (bb_wifi_classify_lifecycle() maps GOT_IP -> START, see bb_wifi.h).
// Deliberately NOT bb_settings_wifi_set_provisioned_cb(): that callback only
// fires on the provisioned flag's false->true transition (B1-807), so
// re-provisioning an ALREADY-provisioned board via
// bb_wifi_prov_request_portal() would never trip it (the flag is already
// true) and the board would never reboot. The "wifi" service's RUNNING edge
// fires on GOT_IP regardless of the provisioned flag's prior value, so it
// covers both first-boot and re-provisioning identically -- and floor
// already owns this edge for the mqtt/mdns wiring above, so no new seam is
// needed.
//
// Only acts when s_prov_mode_boot is true (this boot started with no
// creds, so bb_mdns_init()/bb_mqtt_client_init_default() below were
// skipped) -- a normal boot's GOT_IP must not reboot the device. Fires
// synchronously, outside bb_lifecycle's lock; MUST NOT call any
// bb_lifecycle mutator from here -- arming a bb_timer is not a lifecycle
// mutator either way.
//
// HIGH review fix: the guard itself is floor_should_trigger_prov_reboot()
// (floor_prov_reboot.h) -- a pure function, 100% line/branch host-tested in
// test/test_host/test_floor_prov_reboot.c -- so this, the only thing
// preventing a boot loop, is no longer covered by a single golden HW path
// alone.
//
// HW-CONFIRMED fix: this observer only DECIDES and arms the shared
// bb_system_restart_deferred primitive (B1-refactor extraction of this
// file's former private prov_reboot_work_fn/s_prov_reboot_timer pair,
// mirroring bb_wifi.c's bb_wifi_reconfigure() precedent) -- it must not call
// bb_system_restart_reason() itself. That call does a synchronous NVS flash
// write (nvs_open/nvs_set_str/nvs_commit/nvs_close) followed by
// esp_restart(); run inline here it executes on the shared ESP-IDF default
// event loop ("sys_evt") task, the same task every other wifi/http/mdns/mqtt
// observer in this file also runs on -- and HW showed that racing pair losing
// both the reboot record and the confirming log line (see the module header
// block's HW-CONFIRMED note). A plain vTaskDelay() here is not the fix either
// -- that would block sys_evt for no reason (LOW review fix from a prior
// revision, which is why one was removed). bb_system_restart_deferred's
// work_fn runs on the shared bb_timer_disp task, not sys_evt and not the
// esp_timer service task -- that's the fix.
static void wifi_lifecycle_observer(const bb_lifecycle_event_t *evt, void *user)
{
    (void)user;
    if (!floor_should_trigger_prov_reboot(s_prov_mode_boot, s_prov_reboot_triggered,
                                          evt->svc, s_wifi_svc,
                                          (bb_lifecycle_state_t)evt->old_state,
                                          (bb_lifecycle_state_t)evt->new_state)) {
        return;
    }
    s_prov_reboot_triggered = true;

    // Credentials just proved themselves (a real connect reached GOT_IP).
    // Provisioning is transient by design (see the module header block for
    // the full rationale): this boot never started mdns/mqtt and never will
    // -- the only way to bring up the full stack with the correct hostname
    // is a fresh boot, which now has creds and takes the normal path. The
    // portal's HTTP response and captive-portal teardown are long done by
    // GOT_IP time: the /save handler fully writes its HTML confirmation
    // before returning (bb_wifi_prov.c's prov_save_handler), and the
    // manager's WP_CLOSING settle timer tears down the portal (AP + routes)
    // well before the reconnect FSM it hands off to ever reaches GOT_IP
    // (see wifi_prov_policy.c's "WHY THE SETTLE STATE" block) -- so there is
    // nothing in flight to wait on for the portal's own sake. The 500ms
    // delay below (matching bb_wifi.c's own reconfigure-reboot precedent) is
    // for a different reason: it gets the blocking flash write and
    // esp_restart() off the shared sys_evt task, onto bb_timer_disp instead,
    // and gives bb_log's buffered writer task (see bb_log.c's
    // s_writer_task_fn) time to actually drain and print the log line just
    // below before the CPU halts -- so the reboot reason is observable both
    // in NVS and on the serial console.
    bb_log_w(TAG, "provisioning: credentials validated (got IP) -- "
             "restarting into normal boot");
    // LOW review fix (predates the HW-CONFIRMED timer fix): a dedicated
    // reason -- BB_RESET_SRC_WIFI_RECONFIGURE is also used by bb_wifi.c's
    // existing runtime-reconfigure reboot, which would otherwise conflate
    // two distinct reboot causes in reboot-reason telemetry (weakening the
    // SSOT, B1-532).
    bb_system_restart_deferred(BB_RESET_SRC_WIFI_PROVISIONED, "provisioning validated", 500 * 1000);
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
            // PR6 (floor HW-validation gate for the diag/storage HTTP
            // surface): the legacy exact-route panic/coredump/boot/heap/
            // tasks/sockets handlers (bb_diag_routes_init(), gated
            // CONFIG_BB_DIAG_ROUTES, default y) plus the backend-agnostic
            // bb_storage DELETE/factory-reset routes (bb_storage_http.h,
            // dissolved into bb_diag_http by B1-1154) -- all server-handle
            // routes, so all three stay here on the RUNNING-entry edge
            // alongside bb_diag_sections_init above.
            //
            // CAVEAT (unlike bb_diag_sections_init's registrations, which
            // are one-time and hoisted to app_main() for exactly this
            // reason): these three calls register one-time, non-idempotent
            // state under the hood (bb_cache_register, bb_diag_boot_bind,
            // an initial diag_boot_publish, a clear-on-read NVS
            // reboot-record load) inside a branch this file's own header
            // comment says "re-fires on every pause/resume." That's safe
            // ONLY because floor never pauses/resumes the "http" lifecycle
            // service today. If that changes, these three must move to a
            // call-once path (composition-time, like bb_diag_sections_init's
            // registrations) -- tracked separately.
            route_err = bb_diag_routes_init(server);
            if (route_err != BB_OK) {
                bb_log_w(TAG, "diag_routes_init failed (%d)", (int)route_err);
            }
            route_err = bb_storage_http_routes_init(server);
            if (route_err != BB_OK) {
                bb_log_w(TAG, "storage_http_routes_init failed (%d)", (int)route_err);
            }
            route_err = bb_storage_http_factory_reset_routes_init(server);
            if (route_err != BB_OK) {
                bb_log_w(TAG, "storage_http_factory_reset_routes_init failed (%d)", (int)route_err);
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
            // B1-1215: registers the handler AND the OpenAPI route descriptor
            // (previously this called bb_http_register_route() directly with
            // no descriptor, so /api/events was absent from
            // /api/openapi.json).
            route_err = bb_data_http_espidf_routes_init(server);
            if (route_err != BB_OK) {
                bb_log_w(TAG, "data_http_espidf_routes_init failed (%d)", (int)route_err);
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

    // Starts the periodic base-scan (PRE_HTTP tier, called directly here per
    // floor's handwire convention) that populates bb_task_base_foreach()'s
    // free_bytes -- the SSOT task_stack_log_tick's report below reads. Must
    // run before that timer's first tick has anything meaningful to report;
    // a no-op (returns BB_OK) if CONFIG_FREERTOS_USE_TRACE_FACILITY is off.
    bb_err_t task_scan_err = bb_task_registry_base_scan_start();
    if (task_scan_err != BB_OK) {
        bb_log_w(TAG, "task_registry_base_scan_start failed (%d)", (int)task_scan_err);
    }

    // Baseline reading: HTTP stopped, WiFi not yet started.
    bb_serialize_console_heap_report("baseline-http-stopped");

    // ---------------------------------------------------------------------
    // B1-1045 PR-4 cutover -- INIT-ORDER HAZARD: register the "wifi"
    // bb_lifecycle service and repoint bb_wifi's emit seam onto it BEFORE
    // bb_wifi_autoinit() runs below. bb_wifi_autoinit() can fire its first
    // GOT_IP synchronously from the connect path; if the trampoline
    // binding/emit seam is not wired yet, that first edge is silently
    // dropped (no retry -- bb_wifi has no event replay). The "wifi" service
    // is registered here (this PR starts what PR-1 left dormant).
    //
    // Deliberately NOT eagerly bb_lifecycle_start()'d here (fixed alongside
    // this comment -- an earlier revision did, and it was a bug): starting
    // the service at composition time -- before mDNS/MQTT's observers ever
    // attach -- permanently flips it to RUNNING with zero observers present.
    // The real GOT_IP, arriving later via the trampoline (bb_wifi_classify_
    // lifecycle -> BB_LIFECYCLE_ACTION_START -> bb_lifecycle_start()), then
    // hits bb_lifecycle_start()'s idempotent early-return (already started
    // -> no notify, no version bump) and the STOPPED/PAUSED->RUNNING edge
    // never fires for anyone. mDNS (bb_mdns.c mdns_wifi_observer) and MQTT
    // (bb_mqtt_client_espidf.c) both register their observer FIRST and only
    // THEN check bb_wifi_has_ip() to bypass-start if already connected
    // (register-then-check, no eager start required on this end) -- so the
    // service must stay STOPPED here and reach RUNNING only through the
    // genuine edge, matching the "http" service's own register-observer-
    // before-start pattern below.
    // ---------------------------------------------------------------------
    //
    // Provisioning is transient (see wifi_lifecycle_observer's doc below):
    // snapshot whether this boot started with no creds BEFORE anything
    // below can start a connect (bb_wifi_autoinit() further down) or read
    // this flag (the mdns/mqtt skip decision, and wifi_lifecycle_observer's
    // reboot trigger, both further down still).
    s_prov_mode_boot = !bb_settings_wifi_has_creds();

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
    }
    // Register floor's own reboot-trigger observer here, before
    // bb_wifi_autoinit() below can drive the first GOT_IP -- same
    // register-before-start ordering as the mdns/mqtt consumers' own
    // internal observers (see the comment block above).
    err = bb_lifecycle_observe(wifi_lifecycle_observer, NULL);
    if (err != BB_OK) {
        bb_log_w(TAG, "lifecycle_observe(wifi) failed (%d)", (int)err);
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
    // B1-1418 PR-2: register the console format backend too -- this is the
    // gap the tasks-report migration below exists to find. Before this PR,
    // nothing in floor registered BB_FORMAT_CONSOLE: heap_log_tick calls
    // bb_serialize_console_render() directly (bypassing bb_serialize's
    // format-dispatch registry entirely), and the retired
    // bb_serialize_console_tasks_report() did the same internally. Both
    // never needed a registered renderer to work. bb_data_render_rows()
    // (task_stack_log_tick below) does -- it looks up BB_FORMAT_CONSOLE via
    // bb_serialize_format_get_render() and returns BB_ERR_UNSUPPORTED if
    // nothing is registered -- so this call is genuinely load-bearing, not
    // defensive boilerplate.
    err = bb_serialize_console_register_format();
    if (err != BB_OK) {
        bb_log_w(TAG, "serialize_console_register_format failed (%d)", (int)err);
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
    // Failure is logged by the callee (bb_temp_register_info /
    // bb_mqtt_client_health_register name the specific section that
    // failed); a caller-side log here would only repeat it, so the
    // return is intentionally discarded.
    (void)bb_temp_autoregister_init();
    (void)bb_mqtt_client_health_autoregister_init();
    // B1-1045 PR-4: bind the "log" dissolved-bb_event producer key. Its
    // underlying stash is populated by bb_log_event's own forwarder task
    // (started below, once the http server is up) -- this proves the
    // bb_data bind/render/broadcast closure compiles, links, and serves
    // end to end. The other 5 dissolved-bb_event producer keys stay out of
    // floor's scope (see the file-header comment); smoke's own
    // event-cutover rehab is B1-1051.
    static const struct {
        const char                      *key;
        const bb_serialize_desc_t       *desc;
        const bb_data_plain_fill_ctx_t  *fill_ctx;
    } producers[] = {
        { "log", &bb_log_event_wire_desc, &s_log_fill_ctx },
    };
    for (size_t i = 0; i < sizeof(producers) / sizeof(producers[0]); i++) {
        err = bb_data_bind(&(bb_data_binding_t){
            .key = producers[i].key, .desc = producers[i].desc,
            .gather = bb_data_gather_plain, .ctx = (void *)producers[i].fill_ctx });
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

    // B1-1418 PR-2: bind the "tasks" key rows-only (desc/gather/apply all
    // NULL, rows non-NULL) -- a pure table producer with no scalar egress
    // at all, the shape PR-1 (#1221) made legal. Deliberately NOT run
    // through the producers[] loop or bb_data_http_attach_sized() above:
    // this binding has no desc/gather pair for bb_data_http's SSE/WS
    // broadcaster to render (it's console-only, see bb_data_render_rows()'s
    // own doc) -- task_stack_log_tick's own bb_data_render_rows() call is
    // this key's only consumer.
    err = bb_data_bind(&(bb_data_binding_t){ .key = "tasks", .rows = &s_tasks_row_binding });
    if (err != BB_OK) {
        bb_log_w(TAG, "data_bind(tasks) failed (%d)", (int)err);
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
    //
    // Provisioning is transient: a device mid-provisioning must not
    // advertise itself on the network at all (mDNS would publish the
    // MAC-suffixed default hostname -- the stored one from the portal is
    // only durable across a reboot -- and MQTT would connect under a
    // not-yet-final identity). When this boot started with no creds
    // (s_prov_mode_boot, snapshotted above before bb_wifi_autoinit() could
    // run), skip both entirely; wifi_lifecycle_observer above reboots this
    // boot into the normal path -- with creds present -- the moment the
    // just-saved credentials prove themselves via GOT_IP, and mdns/mqtt come
    // up correctly on that next boot instead.
    if (!s_prov_mode_boot) {
        err = bb_mqtt_client_init_default();
        if (err != BB_OK) {
            bb_log_w(TAG, "mqtt_client_init_default failed (%d)", (int)err);
        }
        // Wire the stored hostname into bb_mdns BEFORE bb_mdns_init() below --
        // bb_mdns_init() is a bb_once (idempotent first-call-wins), and
        // mdns_init_impl() reads s_mdns_hostname/s_mdns_hostname_set exactly
        // once, at that first call, to build the label mdns_hostname_set()
        // publishes -- calling bb_mdns_set_hostname() any later has no effect on
        // the already-published mDNS hostname. This wiring lives HERE, in the
        // composition root, rather than inside bb_mdns itself: bb_mdns is a
        // primitive with no bb_settings dependency today (see its own
        // CMakeLists.txt), and adding one would break the composition-only
        // model (CLAUDE.md) -- breadboard ships primitives, consumers wire them
        // together via codegen/handwire. A missing/empty stored hostname is a
        // silent no-op: bb_settings_hostname_get() returns "" on unset, and
        // bb_mdns's own mdns_build_hostname() already falls back to its
        // MAC-derived "bsp-device-xxxx" default whenever the injected hostname
        // is unset or empty (see bb_mdns_set_hostname()'s doc).
        char hostname[BB_MDNS_HOSTNAME_MAX];
        if (bb_settings_hostname_get(hostname, sizeof(hostname), NULL) == BB_OK) {
            bb_mdns_set_hostname(hostname);
        }
        bb_mdns_init();
    } else {
        bb_log_i(TAG, "provisioning: no creds this boot -- skipping mdns/mqtt bring-up");
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

    // B1-809 validation vehicle: bb_wifi_prov_autoinit() is a no-op
    // returning BB_OK when creds already exist (see its doc), so this call
    // is safe on every boot. It runs here -- after the "http" lifecycle
    // service above has registered floor's own specific GET routes --
    // because bb_wifi_prov_start() registers the captive `GET /*` wildcard,
    // and httpd_uri_match_wildcard() matches in registration order: a
    // first-match wildcard registered before floor's routes would shadow
    // them for the entire provisioning window (see bb_http.c and
    // bb_wifi_prov.h's registration-order invariant). Ordering after
    // bb_storage_nvs_register() and bb_wifi_ensure_net_stack() above still
    // holds, since those satisfy bb_wifi_ap_start()'s prerequisites.
    const bb_http_asset_t *prov_asset = bb_wifi_prov_default_form_get();
    err = bb_wifi_prov_autoinit(prov_asset, 1, NULL);
    if (err != BB_OK) {
        bb_log_w(TAG, "wifi_prov_autoinit failed (%d)", (int)err);
    }

    // Delta reading: provisioning (potentially) active, layered on top of
    // the with-http-running baseline above. When creds already exist,
    // autoinit is a no-op and this reading is expected to simply equal the
    // previous one -- that is fine, not a bug.
    bb_serialize_console_heap_report("with-prov-running");

    bb_periodic_timer_t heap_log_timer =
        start_periodic_log_timer(heap_log_tick, "heap_log", FLOOR_HEAP_LOG_INTERVAL_MS);
    (void)heap_log_timer;

    bb_periodic_timer_t task_stack_log_timer =
        start_periodic_log_timer(task_stack_log_tick, "task_stack_log", FLOOR_TASK_STACK_LOG_INTERVAL_MS);
    (void)task_stack_log_timer;

    // app_main returns; heap-log and task-stack-log jobs run on the shared
    // bb_timer_disp task.
}
