// smoke_data_notify -- B1-1451 (epic B1-1123) on-device proof harness,
// examples/smoke ONLY (never a library component -- mirrors
// smoke_core_claim.c's own shape and rationale).
//
// bb_data_http_notify_push() (components/bb_data_http/include/bb_data_http.h)
// is a library primitive with no in-tree caller until this file: this is
// the "real second caller" B1-1449/B1-1450's own doc comments say is owed.
// It wires ONE bb_data EVENT-kind key ("smoke_notify", topic "smoke.notify")
// and three runtime-triggered POST routes so both halves of B1-1451's DoD
// can be verified OFF-BOARD, with no on-device instrumentation needed
// beyond a serial log line and the existing GET /api/events SSE surface:
//
//   1. DELIVERY LATENCY (notify wakes the broadcaster immediately, vs the
//      broadcaster's own next scheduled BB_DATA_HTTP_SWEEP_INTERVAL_MS
//      sweep): POST /api/smoke/notify-push vs POST /api/smoke/tick-push are
//      an A/B pair -- both push one EVENT for the SAME key, the first via
//      bb_data_http_notify_push() (push + wake), the second via
//      bb_data_touch() alone (push, no wake, next-sweep-only delivery).
//      Off-board verification recipe:
//        curl -N 'http://<board>/api/events?topic=smoke.notify' &
//        T0=$(date +%s.%N); curl -s -X POST http://<board>/api/smoke/notify-push
//      Compare arrival wall-clock time (curl -N's own receive time) against
//      T0: the notify-push frame should arrive within ordinary WiFi RTT (a
//      few to a few tens of ms), while POSTing tick-push instead can take up
//      to BB_DATA_HTTP_SWEEP_INTERVAL_MS (default 50ms) longer, since
//      nothing wakes the broadcaster early for it -- the observable
//      difference IS the claim, measured entirely on the OFF-BOARD (curl)
//      clock, with no on-device clock correlation required.
//
//   2. TWO-TASK RING CONTENTION (the stress exercise B1-1450's own doc
//      flags as "still owed once a real second caller exists"):
//      POST /api/smoke/notify-stress starts a bounded-duration
//      (SMOKE_DATA_NOTIFY_STRESS_MS) tight loop calling
//      bb_data_http_notify_push() as fast as the scheduler allows, on a
//      dedicated task running CONCURRENTLY with the broadcaster task's own
//      unconditional periodic sweep (which pumps the SAME "smoke_notify"
//      key as part of its own EVENT ring-feed step, see
//      bb_data_http_push_pump()'s doc) -- genuine SMP contention on the
//      shared ring's claim/render/commit-or-revoke protocol, both tasks
//      core=ANY (unpinned). The "pushes" counter is on-device (serial log
//      line at completion); "discards" and "anomalies" are derived
//      OFF-BOARD by curling the SSE stream throughout the burst and
//      counting/ordering the received seq values -- see
//      smoke_notify_stress_handler()'s doc below for the exact recipe. This
//      is only a trustworthy signal because s_smoke_notify_payload itself
//      (below) is synchronized -- an unsynchronized producer buffer would
//      let a torn read masquerade as the very protocol failure this stress
//      test exists to detect.
//
// Gated by CONFIG_BB_SMOKE_DATA_NOTIFY (default n) -- EVERYTHING in this
// file, including the "smoke_notify" bb_data binding/attach itself
// (smoke_data_notify_bind()/_attach(), below), is a no-op BB_OK stub when
// the knob is off, so a default smoke build never binds or attaches this
// key at all (B1-1451 review fix -- previously the bind/attach ran
// unconditionally). bb_wire.h's markers call these three entry points
// UNCONDITIONALLY (mirroring smoke_core_claim.c's always-called/
// internally-gated shape) since `bbtool codegen` markers have no Kconfig
// gate of their own -- every symbol this file's owning markers name must
// exist on every smoke build regardless of this knob, hence the wrapper
// functions rather than calling bb_data_bind()/bb_data_http_attach_sized()
// straight from a marker's `args=`.
#include "sdkconfig.h"
#include "smoke_data_notify.h"

#if CONFIG_BB_SMOKE_DATA_NOTIFY

#include "bb_clock.h"
#include "bb_data.h"
#include "bb_data_http.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_log_event_wire.h"
#include "bb_task.h"

// vTaskDelete() (notify_stress_task()'s own self-delete, below) + portMUX
// critical sections (s_payload_mux/s_stress_mux) -- explicit rather than
// relying on some other included header pulling these in transitively,
// mirroring smoke_core_claim.c's own explicit FreeRTOS includes.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "smoke_data_notify";

#define SMOKE_DATA_NOTIFY_KEY   "smoke_notify"
#define SMOKE_DATA_NOTIFY_TOPIC "smoke.notify"

// Bounded stress-burst duration -- fixed rather than query-parsed: a single
// documented window is simpler to verify off-board, and this demo has no
// other reason to vary it.
#define SMOKE_DATA_NOTIFY_STRESS_MS 3000

#define SMOKE_DATA_NOTIFY_STRESS_STACK 3072
// Strictly BELOW BB_DATA_HTTP_TASK_PRIORITY's default (4 --
// platform/espidf/bb_data_http/bb_data_http_espidf.c) -- deliberately, not
// incidentally (B1-1451 review fix, was 5). This task's whole purpose is to
// contend with the broadcaster's OWN claim/render calls on the shared ring,
// never to starve the broadcaster outright. bb_task_yield() (used in the
// loop below) is a bare taskYIELD() (platform/espidf/bb_task/
// bb_task_espidf.c), which only reschedules among tasks at the SAME OR
// HIGHER priority -- it never yields DOWN to a lower-priority task. At the
// previous value (5, matching esp_http_server's own default worker
// priority), an active SSE client's httpd worker plus this stress task
// could occupy both cores at priority 5 indefinitely: notify_push()'s
// xTaskNotifyGive() makes the priority-4 broadcaster READY, not RUNNING,
// and nothing would ever preempt down to it. Priority 3 makes the scheduler
// preempt this task in the broadcaster's favor the instant it's ready,
// regardless of core placement -- contention on the ring stays real (this
// task still races the broadcaster's push_pump() calls), but the
// broadcaster itself is never starved of CPU.
#define SMOKE_DATA_NOTIFY_STRESS_PRIORITY 3

// Producer-owned payload buffer + sequence counter, written by
// smoke_notify_touch() (httpd worker threads for notify-push/tick-push, or
// notify_stress_task()'s own tight loop -- potentially TWO different
// producer contexts racing each other too) and READ by smoke_notify_fill()
// (the render_fn bb_data_http_push_pump() calls UNLOCKED -- see
// bb_data_http_common.c's "Step 3: RENDER, unlocked" -- from WHICHEVER task
// currently owns that push's claim, which can be the broadcaster task on
// the OTHER core while a producer is mid-snprintf() here).
//
// A concurrent write and read of the same bytes is a genuine data race
// (torn/garbled content, UB) that the ring's own coalesce-to-latest
// contract does NOT cover: that contract governs which GENERATION of a
// push wins, not whether the byte content under a single generation can
// tear mid-copy (B1-1451 review fix -- an earlier version of this file
// wrongly conflated the two). s_payload_mux (a short portMUX critical
// section, same idiom platform/espidf/bb_data_http/bb_data_http_espidf.c's
// own side tables use) protects the sequence-counter read-modify-write, the
// snprintf() write, and smoke_notify_fill()'s strncpy() copy-out as ONE
// critical section each -- nothing that can block runs while held, and no
// real work happens inside it beyond the copy itself.
static char          s_smoke_notify_payload[BB_LOG_EVENT_LOG_TEXT_MAX];
static uint32_t      s_seq;
static portMUX_TYPE  s_payload_mux = portMUX_INITIALIZER_UNLOCKED;

// bb_data_plain_fill_fn (bb_data.h) -- copies the most recently formatted
// s_smoke_notify_payload into bb_data's snapshot buffer at render time. See
// s_smoke_notify_payload's own doc above for why this copy is lock-guarded.
static bb_err_t smoke_notify_fill(void *dst)
{
    bb_log_event_wire_t *w = (bb_log_event_wire_t *)dst;
    portENTER_CRITICAL(&s_payload_mux);
    strncpy(w->log, s_smoke_notify_payload, sizeof(w->log) - 1);
    portEXIT_CRITICAL(&s_payload_mux);
    w->log[sizeof(w->log) - 1] = '\0';
    return BB_OK;
}

// Internal now (B1-1451 review fix) -- bb_wire.h no longer references this
// by address directly; smoke_data_notify_bind() below wraps the
// bb_data_bind() call that needs it, so this can stay static.
static const bb_data_plain_fill_ctx_t s_smoke_notify_fill_ctx = {
    .fill = smoke_notify_fill,
};

// Guards notify_stress_task() re-entry: read+set from httpd worker threads
// (smoke_notify_stress_handler(), possibly concurrently on two different
// workers) and cleared from the stress task itself just before
// self-deleting -- a genuinely shared flag across tasks, so both the read
// and the write go under s_stress_mux. Declared ahead of
// notify_stress_task() below since that task clears it.
static bool         s_stress_running;
static portMUX_TYPE s_stress_mux = portMUX_INITIALIZER_UNLOCKED;

// Formats the next sequence number + mode tag into s_smoke_notify_payload
// and bumps bb_data's generation counter for SMOKE_DATA_NOTIFY_KEY -- the
// PUSH half shared by all three handlers below. Whether/how the caller also
// wakes the broadcaster is decided by the caller, not here.
static uint32_t smoke_notify_touch(const char *mode)
{
    uint32_t seq;
    portENTER_CRITICAL(&s_payload_mux);
    seq = ++s_seq;
    snprintf(s_smoke_notify_payload, sizeof(s_smoke_notify_payload),
             "seq=%" PRIu32 " mode=%s", seq, mode);
    portEXIT_CRITICAL(&s_payload_mux);
    bb_data_touch(SMOKE_DATA_NOTIFY_KEY);
    return seq;
}

// POST /api/smoke/notify-push -- pushes ONE EVENT via
// bb_data_http_notify_push() (push + immediate broadcaster wake). See this
// file's header comment for the off-board A/B latency-measurement recipe
// against /api/smoke/tick-push below.
bb_err_t smoke_notify_push_handler(bb_http_request_t *req)
{
    uint32_t seq = smoke_notify_touch("notify");
    bb_data_http_notify_push();
    bb_log_i(TAG, "notify-push: seq=%" PRIu32 " (broadcaster woken)", seq);
    return bb_http_resp_no_content(req);
}

// POST /api/smoke/tick-push -- pushes ONE EVENT via bb_data_touch() alone,
// deliberately WITHOUT waking the broadcaster: delivery waits for the
// broadcaster's own next scheduled BB_DATA_HTTP_SWEEP_INTERVAL_MS sweep.
// The "before" half of the notify-push A/B comparison (this file's header
// comment).
bb_err_t smoke_tick_push_handler(bb_http_request_t *req)
{
    uint32_t seq = smoke_notify_touch("tick");
    bb_log_i(TAG, "tick-push: seq=%" PRIu32 " (no wake -- next sweep only)", seq);
    return bb_http_resp_no_content(req);
}

// Two-task ring-contention stress (this file's header comment, item 2): a
// bounded-duration tight loop calling bb_data_http_notify_push() as fast as
// possible, concurrently with the broadcaster task's own unconditional
// sweep (which pumps this SAME key too -- see bb_data_http_push_pump()'s
// MULTI-WRITER CONTRACT doc, bb_data_http.h). bb_task_yield() between
// iterations (not a real delay) keeps this from starving same-priority/
// same-core work for the whole burst while still running as fast as the
// scheduler allows -- see SMOKE_DATA_NOTIFY_STRESS_PRIORITY's own doc for
// why this task's priority, not just this yield, is what actually keeps
// the broadcaster fed.
static void notify_stress_task(void *arg)
{
    (void)arg;
    uint32_t start_ms  = bb_clock_now_ms();
    uint32_t attempts  = 0;
    while ((bb_clock_now_ms() - start_ms) < SMOKE_DATA_NOTIFY_STRESS_MS) {
        smoke_notify_touch("stress");
        bb_data_http_notify_push();
        attempts++;
        bb_task_yield();
    }
    // "attempts" is the on-device half of the DoD's counter requirement.
    // "discards" (attempts minus DISTINCT delivered seq values -- expected
    // under this component's documented coalesce-to-latest contract, see
    // bb_data_http_push_pump()'s SUSTAINED-CHURN NOTE) and "anomalies" (any
    // received seq <= a previously received seq, i.e. an ordering
    // violation the claim/commit protocol is supposed to make impossible)
    // are both derived OFF-BOARD: curl -N the SSE stream
    // (GET /api/events?topic=smoke.notify) for the duration of this burst,
    // parse "seq=N" out of each frame, and check for non-monotonic seq
    // values -- e.g.:
    //   curl -N 'http://<board>/api/events?topic=smoke.notify' \
    //     | grep -o 'seq=[0-9]*' | tee /tmp/smoke_notify_seq.log
    // then compare /tmp/smoke_notify_seq.log's line count (delivered) and
    // monotonicity against this log line's `attempts` value. This is now a
    // trustworthy signal (B1-1451 review fix): s_smoke_notify_payload is
    // lock-guarded, so a garbled/non-monotonic seq observed off-board
    // reflects a genuine ring-protocol issue, not a torn read in this demo
    // harness itself.
    bb_log_i(TAG, "notify-stress: complete attempts=%" PRIu32
             " duration_ms=%" PRIu32 " topic=%s",
             attempts, (uint32_t)SMOKE_DATA_NOTIFY_STRESS_MS, SMOKE_DATA_NOTIFY_TOPIC);
    portENTER_CRITICAL(&s_stress_mux);
    s_stress_running = false;
    portEXIT_CRITICAL(&s_stress_mux);
    vTaskDelete(NULL);
}

// POST /api/smoke/notify-stress -- starts the bounded stress burst above if
// one is not already running (idempotent no-op otherwise -- the
// check-and-set below is a single critical section, so two POSTs landing on
// two httpd worker threads at the same instant cannot both win). Returns
// immediately (202-shaped no-content); read the "notify-stress: complete"
// serial log line, or curl the SSE stream throughout the
// SMOKE_DATA_NOTIFY_STRESS_MS window, for results (see notify_stress_task()'s
// doc above for the exact off-board recipe).
bb_err_t smoke_notify_stress_handler(bb_http_request_t *req)
{
    portENTER_CRITICAL(&s_stress_mux);
    bool already_running = s_stress_running;
    if (!already_running) {
        s_stress_running = true;
    }
    portEXIT_CRITICAL(&s_stress_mux);
    if (already_running) {
        bb_log_w(TAG, "notify-stress: already running, ignoring");
        return bb_http_resp_no_content(req);
    }

    bb_task_config_t cfg = {
        .entry       = notify_stress_task,
        .name        = "smoke_notify_stress",
        .arg         = NULL,
        .stack_bytes = SMOKE_DATA_NOTIFY_STRESS_STACK,
        .priority    = SMOKE_DATA_NOTIFY_STRESS_PRIORITY,
        .core        = BB_TASK_CORE_ANY,
        .backing     = BB_TASK_BACKING_DYNAMIC,
    };
    void *handle = NULL;
    bb_err_t err = bb_task_create(&cfg, &handle);
    if (err != BB_OK) {
        bb_log_e(TAG, "notify-stress task create failed: %d", (int)err);
        portENTER_CRITICAL(&s_stress_mux);
        s_stress_running = false;
        portEXIT_CRITICAL(&s_stress_mux);
        return err;
    }
    bb_log_i(TAG, "notify-stress: started (duration_ms=%d)", SMOKE_DATA_NOTIFY_STRESS_MS);
    return bb_http_resp_no_content(req);
}

// Wraps bb_data_bind() -- see this file's header comment for why this is a
// wrapper (rather than bb_wire.h calling bb_data_bind() straight from a
// marker's `args=`, as the "log" key's own marker does): it is what lets
// the "smoke_notify" binding be skipped entirely when
// CONFIG_BB_SMOKE_DATA_NOTIFY is off.
bb_err_t smoke_data_notify_bind(void)
{
    return bb_data_bind(&(bb_data_binding_t){
        .key    = SMOKE_DATA_NOTIFY_KEY,
        .desc   = &bb_log_event_wire_desc,
        .gather = bb_data_gather_plain,
        .ctx    = (void *)&s_smoke_notify_fill_ctx,
    });
}

// Wraps bb_data_http_attach_sized() -- see smoke_data_notify_bind()'s doc
// immediately above for the gating rationale.
bb_err_t smoke_data_notify_attach(void)
{
    return bb_data_http_attach_sized(SMOKE_DATA_NOTIFY_KEY, SMOKE_DATA_NOTIFY_TOPIC,
                                     BB_DATA_HTTP_EVENT, bb_log_event_wire_desc.snap_size);
}

bb_err_t smoke_data_notify_routes_init(bb_http_handle_t server)
{
    if (!server) {
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t err = bb_http_register_route(server, BB_HTTP_POST,
                                          "/api/smoke/notify-push",
                                          smoke_notify_push_handler);
    if (err != BB_OK) {
        return err;
    }
    err = bb_http_register_route(server, BB_HTTP_POST,
                                 "/api/smoke/tick-push",
                                 smoke_tick_push_handler);
    if (err != BB_OK) {
        return err;
    }
    return bb_http_register_route(server, BB_HTTP_POST,
                                  "/api/smoke/notify-stress",
                                  smoke_notify_stress_handler);
}

#else /* CONFIG_BB_SMOKE_DATA_NOTIFY not set */

bb_err_t smoke_data_notify_bind(void)
{
    return BB_OK;
}

bb_err_t smoke_data_notify_attach(void)
{
    return BB_OK;
}

bb_err_t smoke_data_notify_routes_init(bb_http_handle_t server)
{
    (void)server;
    return BB_OK;
}

#endif
