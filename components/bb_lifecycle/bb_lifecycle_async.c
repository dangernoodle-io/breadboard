// bb_lifecycle — async observer dispatch (B1-1034). Always compiled --
// the always-built sync core (bb_lifecycle.c) has no platform/queue/task
// dependency of its own and calls into this file's notify/invoke seam
// unconditionally.
//
// One shared queue + one shared drain task, lazily created on the FIRST
// bb_lifecycle_observe_async() call. The drain task never mutates lifecycle
// state itself -- it only invokes async-flagged observer callbacks, which
// (like sync observers) must not call a lifecycle mutator.
//
// Init is mutex-guarded (bb_lock, ensure_async_lock() mirrors ensure_lock()'s
// bb_once-guarded-init pattern in bb_lifecycle.c -- the lock ITSELF is a
// permanent one-shot singleton, safe to latch forever) rather than a
// permanent bb_once latch on the spawn attempt itself (B1-1044): a TRANSIENT
// spawn failure (bb_bqueue_create()/bb_task_create() under pool exhaustion or
// heap pressure) must be retryable by a LATER bb_lifecycle_observe_async()
// call, not permanently poison async dispatch for the rest of the process.
// s_async_ready only ever transitions UNINIT -> READY (on success) under
// s_async_lock; a failed attempt leaves it UNINIT so the next caller to take
// the lock genuinely re-attempts async_init() against a freshly-freed pool.
#include "bb_lifecycle.h"
#include "bb_lifecycle_priv.h"

#include "bb_bqueue.h"
#include "bb_task.h"
#include "bb_once.h"
#include "bb_lock.h"
#include "bb_log.h"
#include "bb_clock.h"

#include <stdatomic.h>

static const char *TAG = "bb_lifecycle_async";

#ifndef CONFIG_BB_LIFECYCLE_ASYNC_QUEUE_DEPTH
#define CONFIG_BB_LIFECYCLE_ASYNC_QUEUE_DEPTH 8
#endif
#ifndef CONFIG_BB_LIFECYCLE_ASYNC_TASK_STACK
#define CONFIG_BB_LIFECYCLE_ASYNC_TASK_STACK 3072
#endif
#ifndef CONFIG_BB_LIFECYCLE_ASYNC_TASK_PRIORITY
#define CONFIG_BB_LIFECYCLE_ASYNC_TASK_PRIORITY 3
#endif
#ifndef CONFIG_BB_LIFECYCLE_ASYNC_TASK_CORE
#define CONFIG_BB_LIFECYCLE_ASYNC_TASK_CORE BB_TASK_CORE_ANY
#endif

// Drop-log rate limit: at most one warn per this many milliseconds, however
// many drops actually occur in the window (no per-drop log storm). The
// exact count is always still available via bb_bqueue_dropped().
#define BB_LIFECYCLE_ASYNC_DROP_LOG_INTERVAL_MS 5000u

// ---------------------------------------------------------------------------
// Lazy-init state
// ---------------------------------------------------------------------------

// s_async_lock guards s_async_ready/s_async_q/s_async_init_err below --
// permanent one-shot singleton (safe: unlike the spawn attempt it guards,
// initializing the lock itself has no transient-failure retry requirement).
static bb_lock_t s_async_lock;
static bb_once_t s_async_lock_once = BB_ONCE_INIT;

static bool         s_async_ready;    // true only after a SUCCESSFUL async_init() -- guarded by s_async_lock
static bb_bqueue_t  s_async_q;
static bb_err_t     s_async_init_err = BB_OK; // result of the most recent init attempt

static void init_async_lock(void *ctx)
{
    (void)ctx;
    bb_lock_config_t cfg = { .name = "bb_lifecycle_async", .category = "service" };
    bb_lock_init(&cfg, &s_async_lock);
}

static void ensure_async_lock(void)
{
    bb_once_run(&s_async_lock_once, init_async_lock, NULL);
}

// Rate-limit timestamp for the drop-log warn below. bb_lifecycle_priv_
// async_notify() runs concurrently from multiple producer tasks/cores (after
// s_lock is released), so a full queue can be hit by two+ producers at once
// -- a plain read-then-write here would let every one of them log in the
// same window. A single _Atomic int64_t + CAS makes exactly one producer the
// "winner" per window (lock-free, no new lock in the enqueue path). Sentinel
// -1 means "never logged yet"; every real bb_clock_now_ms() value is
// non-negative once widened to int64_t, so it can never collide with the
// sentinel.
#define BB_LIFECYCLE_ASYNC_DROP_LOG_UNSET ((int64_t)-1)
static _Atomic int64_t s_drop_log_last_ms = BB_LIFECYCLE_ASYNC_DROP_LOG_UNSET;

// STATIC task backing -- zero heap. On ESP-IDF these must be real
// StackType_t/StaticTask_t storage; on host bb_task_create() (see
// platform/host/bb_task/bb_task_host.c) never dereferences either buffer,
// so a plain byte array is sufficient there.
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static StaticTask_t s_drain_tcb;
static StackType_t  s_drain_stack[CONFIG_BB_LIFECYCLE_ASYNC_TASK_STACK / sizeof(StackType_t)];
#else
static uint8_t s_drain_tcb[64];
static uint8_t s_drain_stack[CONFIG_BB_LIFECYCLE_ASYNC_TASK_STACK];
#endif

// ---------------------------------------------------------------------------
// Drain loop -- ONE body, shared by the production task entry
// (BB_BQUEUE_WAIT_FOREVER) and the BB_LIFECYCLE_TESTING per-call wrapper
// (caller timeout, so a test pthread can run it in a bounded/joinable loop
// against the REAL host bb_bqueue backend -- host's bb_task_create() never
// actually runs the production task, see bb_lifecycle.h).
// ---------------------------------------------------------------------------

static bb_err_t drain_once(uint32_t timeout_ms)
{
    bb_lifecycle_event_t evt;
    bb_err_t err = bb_bqueue_receive(s_async_q, &evt, timeout_ms);
    if (err != BB_OK) {
        return err;
    }
    bb_lifecycle_priv_invoke_async_slots(&evt);
    return BB_OK;
}

// LCOV_EXCL_START — the actual bb_task_create()-spawned task entry. Host's
// bb_task_create() (platform/host/bb_task/bb_task_host.c) is a fake
// no-thread stub that fabricates a handle and never invokes `entry` at all,
// so this body never runs on host; drain_once() (the entire per-iteration
// body this just loops forever) IS host-covered directly by
// bb_lifecycle_async_test_drain_once() (test_bb_lifecycle_async.c).
static void drain_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        drain_once(BB_BQUEUE_WAIT_FOREVER);
    }
}
// LCOV_EXCL_STOP

static void async_init(void *ctx)
{
    (void)ctx;

    bb_bqueue_cfg_t qcfg = {
        .capacity   = CONFIG_BB_LIFECYCLE_ASYNC_QUEUE_DEPTH,
        .item_bytes = sizeof(bb_lifecycle_event_t),
        .name       = "bb_lifecycle_async",
    };
    bb_err_t err = bb_bqueue_create(&qcfg, &s_async_q);
    // Covered by test_bb_lifecycle_async_observe_async_bqueue_exhausted_
    // propagates_error (test_bb_lifecycle_async.c): bb_bqueue_create()
    // deterministically returns BB_ERR_NO_SPACE once the static pool
    // (BB_BQUEUE_MAX_INSTANCES) is exhausted. s_async_ready is left false on
    // this path (see ensure_async_started()), so the very next
    // bb_lifecycle_observe_async() call genuinely retries -- no reset hook
    // needed for that alone; bb_lifecycle_async_reset_for_test() (below,
    // BB_LIFECYCLE_TESTING-only) exists so a test can also un-latch after
    // freeing the held pool, for full test isolation.
    if (err != BB_OK) {
        s_async_init_err = err;
        return;
    }

    // wdt_arm=false: this task blocks BB_BQUEUE_WAIT_FOREVER by design
    // (mirrors platform/espidf/bb_event/bb_event_espidf.c's "bb_event_disp"
    // dispatcher task, the closest existing precedent for a
    // forever-blocking bb_task_create() consumer).
    bb_task_config_t tcfg = {
        .entry       = drain_task_entry,
        .name        = "bb_lifecycle_asy",
        .arg         = NULL,
        .stack_bytes = CONFIG_BB_LIFECYCLE_ASYNC_TASK_STACK,
        .priority    = CONFIG_BB_LIFECYCLE_ASYNC_TASK_PRIORITY,
        .core        = CONFIG_BB_LIFECYCLE_ASYNC_TASK_CORE,
        .backing     = BB_TASK_BACKING_STATIC,
        .stack_buf   = s_drain_stack,
        .tcb_buf     = &s_drain_tcb,
        .wdt_arm     = false,
    };
    err = bb_task_create(&tcfg, NULL);
    // LCOV_EXCL_START — host's bb_task_create() never fails validation for
    // this call site's fixed, always-valid cfg, so this branch has no
    // deterministic host trigger (unlike the bb_bqueue_create() failure
    // above, forced via real pool exhaustion).
    if (err != BB_OK) {
        bb_bqueue_destroy(s_async_q);
        s_async_q = NULL;
        s_async_init_err = err;
        return;
    }
    // LCOV_EXCL_STOP

    s_async_init_err = BB_OK;
}

// Mutex-guarded retry-safe init (B1-1044): serializes every caller through
// s_async_lock so exactly one task ever runs async_init() at a time (real
// blocking acquire -- no busy-spin). Only a SUCCESSFUL run commits
// s_async_ready=true; a failed run leaves it false, so the very next caller
// to take the lock (whether that's a losing concurrent caller from this
// round or a wholly later bb_lifecycle_observe_async() call) genuinely
// re-attempts async_init(), not a cached replay of the old failure.
static bb_err_t ensure_async_started(void)
{
    ensure_async_lock();
    bb_lock_lock(&s_async_lock);
    if (s_async_ready) {
        bb_lock_unlock(&s_async_lock);
        return BB_OK;
    }

    async_init(NULL); // sets s_async_init_err
    bb_err_t err = s_async_init_err;
    if (err == BB_OK) {
        s_async_ready = true;
    }
    bb_lock_unlock(&s_async_lock);
    return err;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_lifecycle_observe_async(bb_lifecycle_observer_fn cb, void *user)
{
    if (!cb) return BB_ERR_INVALID_ARG;

    bb_err_t err = ensure_async_started();
    // Covered by test_bb_lifecycle_async_observe_async_bqueue_exhausted_
    // propagates_error (test_bb_lifecycle_async.c) -- same first-ever-call
    // pool-exhaustion scenario that exercises async_init()'s bb_bqueue_
    // create() failure branch above; ensure_async_started() propagates
    // s_async_init_err straight through to this call site.
    if (err != BB_OK) {
        return err;
    }
    return bb_lifecycle_priv_observe_slot(cb, user, true);
}

void bb_lifecycle_priv_async_notify(const bb_lifecycle_event_t *evt)
{
    // notify_all() only calls this after confirming at least one async slot
    // is registered, which only happens after a bb_lifecycle_observe_async()
    // call already succeeded -- s_async_q is guaranteed non-NULL here.
    bb_err_t err = bb_bqueue_send(s_async_q, evt, 0); // non-blocking: never per-observer, never blocking
    if (err == BB_OK) {
        return;
    }

    // Rate-limit the warn to at most once per BB_LIFECYCLE_ASYNC_DROP_LOG_
    // INTERVAL_MS, even with multiple producers hitting a full queue at once
    // (see s_drop_log_last_ms's declaration comment): read-check-CAS, so only
    // the CAS winner logs. now32 widened to int64_t is always >= 0, so it can
    // never alias the UNSET sentinel; the actual elapsed-time comparison
    // still uses wrap-safe uint32_t subtraction (bb_clock_now_ms() wraps at
    // 49.7 days).
    uint32_t now32 = bb_clock_now_ms();
    int64_t  now   = (int64_t)now32;
    int64_t  last  = atomic_load(&s_drop_log_last_ms);
    bool     expired = (last == BB_LIFECYCLE_ASYNC_DROP_LOG_UNSET) ||
                       ((now32 - (uint32_t)last) >= BB_LIFECYCLE_ASYNC_DROP_LOG_INTERVAL_MS);
    if (expired && atomic_compare_exchange_strong(&s_drop_log_last_ms, &last, now)) {
        size_t dropped = 0;
        bb_bqueue_dropped(s_async_q, &dropped);
        bb_log_w(TAG, "async queue full, dropping event (total dropped=%u)", (unsigned)dropped);
    }
}

#ifdef BB_LIFECYCLE_TESTING
bb_err_t bb_lifecycle_async_test_drain_once(uint32_t timeout_ms)
{
    if (!s_async_q) {
        return BB_ERR_UNSUPPORTED;
    }
    return drain_once(timeout_ms);
}

size_t bb_lifecycle_async_test_dropped(void)
{
    if (!s_async_q) {
        return 0;
    }
    size_t dropped = 0;
    bb_bqueue_dropped(s_async_q, &dropped);
    return dropped;
}

void bb_lifecycle_async_reset_for_test(void)
{
    ensure_async_lock();
    bb_lock_lock(&s_async_lock);
    if (s_async_q) {
        bb_bqueue_destroy(s_async_q);
        s_async_q = NULL;
    }
    s_async_ready = false; // un-latch: next observe_async() re-attempts init
    s_async_init_err = BB_OK;
    bb_lock_unlock(&s_async_lock);
    atomic_store(&s_drop_log_last_ms, BB_LIFECYCLE_ASYNC_DROP_LOG_UNSET);
}
#endif

#ifdef BB_LIFECYCLE_TESTING
void bb_lifecycle_async_drain_dispatch_for_test(const bb_lifecycle_event_t *evt)
{
    bb_lifecycle_priv_invoke_async_slots(evt);
}
#endif
