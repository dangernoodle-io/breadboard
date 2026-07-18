// Tests for bb_lifecycle's optional async observer dispatch (B1-1034).
// Host build MUST compile with BB_LIFECYCLE_ASYNC=1 (see platformio.ini) or
// every path below is compiled out and uncovered.
//
// Concurrency-test hygiene (see test_bb_bqueue.c / test_bb_tcp_client.c for
// the same precedent): NEVER TEST_ASSERT while a worker pthread is still
// live -- Unity's failure path longjmps out without joining, orphaning the
// thread. Every pthread test here records observations into plain/atomic
// locals, JOINS every worker, and only THEN runs TEST_ASSERT. Where a
// property is provable without real concurrency (FIFO order, full-queue
// drop, dispatch-in-isolation), the test below is single-threaded and
// deterministic instead of racing threads for it.
#include "unity.h"
#include "bb_lifecycle.h"
#include "bb_bqueue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

// Mirrors CONFIG_BB_LIFECYCLE_MAX_OBSERVERS's Kconfig C default (see
// test_bb_lifecycle.c's identical TEST_MAX_OBSERVERS convention) -- this is
// a distinct translation unit so the constant is re-declared here rather
// than shared.
#define TEST_MAX_OBSERVERS 8

extern void test_bb_lifecycle_reset_local(void); // test_bb_lifecycle.c

// Drains the shared async singleton queue empty (discarding whatever it
// dispatches to -- test_bb_lifecycle_reset_local() already cleared the
// observer table, so nothing fires) so a leftover item from a prior test
// can never leak into this one. Safe even before the queue exists
// (BB_ERR_UNSUPPORTED short-circuits immediately).
static void drain_async_queue_empty(void)
{
    while (bb_lifecycle_async_test_drain_once(0) == BB_OK) {
        // keep draining
    }
}

static void reset_world(void)
{
    test_bb_lifecycle_reset_local();
    drain_async_queue_empty();
}

// ---------------------------------------------------------------------------
// Pre-init state: bb_lifecycle_async_test_dropped() before the FIRST-EVER
// bb_lifecycle_observe_async() call anywhere in this test binary. MUST be
// the first RUN_TEST() in this file's block (test_main.c) -- the shared
// async queue is a bb_once-guarded, process-lifetime singleton (see
// bb_lifecycle_async.c), so once any later test below successfully calls
// bb_lifecycle_observe_async(), this pre-init state can never be observed
// again for the rest of the process.
// ---------------------------------------------------------------------------

void test_bb_lifecycle_async_test_dropped_before_any_init_returns_zero(void)
{
    TEST_ASSERT_EQUAL_UINT((size_t)0, bb_lifecycle_async_test_dropped());
}

// ---------------------------------------------------------------------------
// (h) bb_bqueue_create() failure propagation: MUST also run before any other
// test in this file successfully calls bb_lifecycle_observe_async() -- once
// that first-ever call succeeds, the shared async queue is a bb_once-guarded,
// process-lifetime singleton and this failure can never be forced again in
// this binary. Exhausts the bb_bqueue static pool with plain bb_bqueue_
// create() calls (no dependence on the exact BB_BQUEUE_MAX_INSTANCES value)
// so bb_lifecycle_async's OWN first-ever bb_bqueue_create() call (inside
// async_init()) fails deterministically with BB_ERR_NO_SPACE, which
// ensure_async_started()/bb_lifecycle_observe_async() must propagate as-is
// (NOT BB_OK, NOT BB_ERR_UNSUPPORTED -- that's the gate-off stub's error).
// ---------------------------------------------------------------------------

#define POOL_EXHAUST_MAX_HELD 8

static void pool_exhaust_observer(const bb_lifecycle_event_t *evt, void *user)
{
    (void)evt;
    (void)user; // never actually invoked -- registration itself must fail
}

void test_bb_lifecycle_async_observe_async_bqueue_exhausted_propagates_error(void)
{
    bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = 1, .name = "exhaust" };
    bb_bqueue_t held[POOL_EXHAUST_MAX_HELD];
    int held_count = 0;
    while (held_count < POOL_EXHAUST_MAX_HELD &&
           bb_bqueue_create(&cfg, &held[held_count]) == BB_OK) {
        held_count++;
    }
    TEST_ASSERT_TRUE(held_count > 0); // pool is finite -- must have exhausted it

    bb_err_t err = bb_lifecycle_observe_async(pool_exhaust_observer, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);          // the propagated bb_bqueue failure
    TEST_ASSERT_NOT_EQUAL(BB_ERR_UNSUPPORTED, err);   // not the gate-off stub's error
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);

    // Clean up: free every held instance AND un-latch bb_lifecycle_async's
    // own ready-state/init-error/queue-handle, so every later test's first
    // bb_lifecycle_observe_async() call gets a genuine (re-)attempt against a
    // freshly-freed pool instead of replaying this failure forever.
    bb_bqueue_test_reset();
    bb_lifecycle_async_reset_for_test();
}

// ---------------------------------------------------------------------------
// (j) B1-1044: a TRANSIENT bb_bqueue_create() failure must be retryable by a
// LATER bb_lifecycle_observe_async() call, WITHOUT the test-only reset hook
// -- proves the fix is in the production init state machine itself, not
// merely in the test scaffolding. Forces the exact same deterministic
// pool-exhaustion failure as the test above, then frees exactly ONE held
// instance (clearing the transient condition) and retries in-line.
// ---------------------------------------------------------------------------

void test_bb_lifecycle_async_observe_async_retries_after_transient_failure(void)
{
    reset_world();
    // Force a clean UNINIT starting point regardless of any earlier test's
    // successful init, so the pool-exhaustion trick below can actually reach
    // async_init()'s bb_bqueue_create() call (a READY singleton would
    // short-circuit ensure_async_started() before ever touching the pool).
    bb_lifecycle_async_reset_for_test();

    bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = 1, .name = "exhaust" };
    bb_bqueue_t held[POOL_EXHAUST_MAX_HELD];
    int held_count = 0;
    while (held_count < POOL_EXHAUST_MAX_HELD &&
           bb_bqueue_create(&cfg, &held[held_count]) == BB_OK) {
        held_count++;
    }
    TEST_ASSERT_TRUE(held_count > 0); // pool is finite -- must have exhausted it

    bb_err_t err = bb_lifecycle_observe_async(pool_exhaust_observer, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err); // transient failure -- NOT latched permanently

    // Clear the transient condition: free exactly one held pool instance.
    bb_bqueue_destroy(held[--held_count]);

    // Retry WITHOUT calling bb_lifecycle_async_reset_for_test() again -- this
    // is the crux of the fix: the init state machine itself re-attempts
    // async_init() because the prior failed attempt left it UNINIT rather
    // than latching a permanent DONE.
    err = bb_lifecycle_observe_async(pool_exhaust_observer, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Clean up: free every remaining held instance plus this test's own
    // successfully-created singleton, so later tests get a fresh pool/state.
    for (int i = 0; i < held_count; i++) {
        bb_bqueue_destroy(held[i]);
    }
    bb_bqueue_test_reset();
    bb_lifecycle_async_reset_for_test();
}

// ---------------------------------------------------------------------------
// (e) drain-dispatch-for-test unit path -- direct, no queue involved.
// ---------------------------------------------------------------------------

static int   s_async_calls;
static bb_lifecycle_event_t s_async_last;
static void *s_async_last_user;

static void async_observer(const bb_lifecycle_event_t *evt, void *user)
{
    s_async_calls++;
    s_async_last = *evt;
    s_async_last_user = user;
}

static int s_sync_calls;
static void sync_observer(const bb_lifecycle_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    s_sync_calls++;
}

static void reset_capture(void)
{
    s_async_calls = 0;
    memset(&s_async_last, 0, sizeof(s_async_last));
    s_async_last_user = NULL;
    s_sync_calls = 0;
}

void test_bb_lifecycle_async_drain_dispatch_for_test_invokes_only_async_slots(void)
{
    reset_world();
    reset_capture();

    int token = 7;
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe(sync_observer, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe_async(async_observer, &token));

    bb_lifecycle_event_t evt = { .svc = 3, .inhibits = 0, .version = 9,
                                  .old_state = BB_LIFECYCLE_STOPPED,
                                  .new_state = BB_LIFECYCLE_RUNNING,
                                  .reason = BB_LIFECYCLE_REASON_NONE, ._pad = 0 };
    bb_lifecycle_async_drain_dispatch_for_test(&evt);

    TEST_ASSERT_EQUAL(1, s_async_calls);
    TEST_ASSERT_EQUAL(0, s_sync_calls); // dispatch-for-test never touches sync slots
    TEST_ASSERT_EQUAL(3, s_async_last.svc);
    TEST_ASSERT_EQUAL(9, s_async_last.version);
    TEST_ASSERT_EQUAL_PTR(&token, s_async_last_user);
}

void test_bb_lifecycle_async_observe_null_cb_invalid_arg(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lifecycle_observe_async(NULL, NULL));
}

void test_bb_lifecycle_async_observer_capacity_overflow_no_space(void)
{
    reset_world();

    bb_err_t err = BB_OK;
    for (int i = 0; i < TEST_MAX_OBSERVERS; i++) {
        err = bb_lifecycle_observe_async(async_observer, NULL);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_lifecycle_observe_async(async_observer, NULL));
}

// ---------------------------------------------------------------------------
// (f) sync-path regression: a sync observer still fires INLINE (no drain
// needed) alongside a registered async observer, proving the notify_all()
// split didn't change default (sync) behavior.
// ---------------------------------------------------------------------------

void test_bb_lifecycle_async_sync_observer_still_fires_inline_unaffected(void)
{
    reset_world();
    reset_capture();

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe(sync_observer, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe_async(async_observer, NULL));

    bb_lifecycle_config_t cfg = { .name = "svc-async-a" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    bb_lifecycle_start(svc);

    // sync observer already ran, inline, before this line -- no drain call
    // has happened yet.
    TEST_ASSERT_EQUAL(1, s_sync_calls);
    TEST_ASSERT_EQUAL(0, s_async_calls); // async observer is still queued, not yet dispatched

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_async_test_drain_once(0));
    TEST_ASSERT_EQUAL(1, s_async_calls); // now dispatched
}

// ---------------------------------------------------------------------------
// (b) FIFO delivery order -- single-threaded, deterministic: drive the REAL
// shared queue via ordinary lifecycle transitions, then drain sequentially
// on this same thread and confirm delivery order matches transition order.
// ---------------------------------------------------------------------------

static uint32_t s_fifo_versions[8];
static int      s_fifo_count;

static void fifo_observer(const bb_lifecycle_event_t *evt, void *user)
{
    (void)user;
    if (s_fifo_count < 8) {
        s_fifo_versions[s_fifo_count++] = evt->version;
    }
}

void test_bb_lifecycle_async_fifo_order_preserved(void)
{
    reset_world();
    s_fifo_count = 0;
    memset(s_fifo_versions, 0, sizeof(s_fifo_versions));

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe_async(fifo_observer, NULL));

    bb_lifecycle_config_t cfg = { .name = "svc-async-b" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    bb_lifecycle_start(svc);                    // transition 1
    bb_lifecycle_pause_assert(svc, "reason-x");  // transition 2
    bb_lifecycle_pause_clear(svc, "reason-x");   // transition 3

    // Three real transitions were enqueued onto the shared queue; drain them
    // sequentially, one per call, on this same (main) thread.
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_async_test_drain_once(0));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_async_test_drain_once(0));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_async_test_drain_once(0));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_lifecycle_async_test_drain_once(0)); // empty now

    TEST_ASSERT_EQUAL(3, s_fifo_count);
    TEST_ASSERT_TRUE(s_fifo_versions[0] < s_fifo_versions[1]);
    TEST_ASSERT_TRUE(s_fifo_versions[1] < s_fifo_versions[2]);
}

// ---------------------------------------------------------------------------
// (c) full-queue drop-and-count -- single-threaded, deterministic: host's
// bb_task_create() never actually drains (fake no-thread stub), so the
// shared queue only ever empties when THIS test explicitly drains it. Fill
// it to CONFIG_BB_LIFECYCLE_ASYNC_QUEUE_DEPTH (host-pinned to 3, see
// platformio.ini) without draining, then confirm one more transition drops.
// bb_lifecycle_async_test_dropped() is a monotonic, never-reset counter, so
// this asserts a BEFORE/AFTER DELTA rather than an absolute value.
// ---------------------------------------------------------------------------

void test_bb_lifecycle_async_full_queue_drops_and_counts(void)
{
    reset_world();

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe_async(async_observer, NULL));

    bb_lifecycle_config_t cfg = { .name = "svc-async-c" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    size_t dropped_before = bb_lifecycle_async_test_dropped();

    // Exactly 3 distinct-reason transitions fill the (host-pinned) capacity
    // of 3 without a single drain in between.
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, "r1"));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, "r2"));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, "r3"));
    // A 4th real transition -- queue is full, dropped (silently, from the
    // emitter's point of view: the transition itself still succeeds).
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, "r4"));

    size_t dropped_after = bb_lifecycle_async_test_dropped();
    TEST_ASSERT_EQUAL_UINT((dropped_before + 1), dropped_after);

    // Clean up: drain the 3 that DID make it in so this test leaves the
    // shared queue empty for whatever runs next.
    drain_async_queue_empty();
}

// ---------------------------------------------------------------------------
// (c2) two drops in the SAME rate-limit window: the first hits the drop-log
// "log" branch, the second (no bb_clock advance in between) hits the
// "suppress" branch -- bb_bqueue_dropped() must still count BOTH, proving the
// rate limit only gates the WARN, never the counter.
// ---------------------------------------------------------------------------

void test_bb_lifecycle_async_full_queue_second_drop_in_window_suppressed(void)
{
    reset_world();

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe_async(async_observer, NULL));

    bb_lifecycle_config_t cfg = { .name = "svc-async-e" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    size_t dropped_before = bb_lifecycle_async_test_dropped();

    // Fill to the (host-pinned) capacity of 3 without draining.
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, "s1"));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, "s2"));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, "s3"));
    // Two more real transitions, back-to-back, both dropped by the full
    // queue -- same rate-limit window, so only the first logs.
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, "s4"));
    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_pause_assert(svc, "s5"));

    size_t dropped_after = bb_lifecycle_async_test_dropped();
    TEST_ASSERT_EQUAL_UINT((dropped_before + 2), dropped_after);

    drain_async_queue_empty();
}

// ---------------------------------------------------------------------------
// (i) bb_lifecycle_async_reset_for_test()'s s_async_q != NULL branch: the
// s_async_q == NULL branch is already covered by
// test_bb_lifecycle_async_observe_async_bqueue_exhausted_propagates_error
// (which calls this reset hook before the queue is ever created). This test
// covers the other half -- destroying a REAL, already-created queue -- and
// proves the once-guard un-latch actually works (the next observe_async()
// call genuinely re-attempts init, not a cached replay).
// ---------------------------------------------------------------------------

void test_bb_lifecycle_async_reset_for_test_destroys_existing_queue(void)
{
    reset_world();

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe_async(async_observer, NULL));

    bb_lifecycle_async_reset_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe_async(async_observer, NULL));

    drain_async_queue_empty();
}

// ---------------------------------------------------------------------------
// (a) async observer callback runs OFF the enqueuing (main) thread. Real
// cross-thread proof: spawn a worker blocked in bb_lifecycle_async_test_
// drain_once() against the REAL host bb_bqueue backend, THEN enqueue from
// the main thread, THEN join, THEN assert (see file header hygiene rules).
// ---------------------------------------------------------------------------

static pthread_t s_observed_thread_id;
static bool      s_observed_thread_id_set;

static void thread_id_observer(const bb_lifecycle_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    s_observed_thread_id = pthread_self();
    s_observed_thread_id_set = true;
}

typedef struct {
    bb_err_t result;
} drain_worker_ctx_t;

static void *drain_worker_fn(void *arg)
{
    drain_worker_ctx_t *ctx = (drain_worker_ctx_t *)arg;
    ctx->result = bb_lifecycle_async_test_drain_once(2000); // blocks up to 2s for one item
    return NULL;
}

void test_bb_lifecycle_async_observer_runs_off_enqueuing_thread(void)
{
    reset_world();
    s_observed_thread_id_set = false;

    TEST_ASSERT_EQUAL(BB_OK, bb_lifecycle_observe_async(thread_id_observer, NULL));

    bb_lifecycle_config_t cfg = { .name = "svc-async-d" };
    bb_lifecycle_svc_t svc;
    bb_lifecycle_register(&cfg, &svc);

    drain_worker_ctx_t ctx = { .result = BB_ERR_INVALID_STATE };
    pthread_t worker;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&worker, NULL, drain_worker_fn, &ctx));

    // Give the worker a moment to genuinely enter bb_bqueue_receive() before
    // the enqueue -- a race either way is harmless here (receive() blocks up
    // to 2s regardless of arrival order), so no sleep-based gate is needed.
    bb_lifecycle_start(svc); // main thread: the ONLY enqueue

    pthread_join(worker, NULL); // ALWAYS join before any TEST_ASSERT below

    pthread_t main_thread_id = pthread_self();
    TEST_ASSERT_EQUAL(BB_OK, ctx.result);
    TEST_ASSERT_TRUE(s_observed_thread_id_set);
    TEST_ASSERT_FALSE(pthread_equal(main_thread_id, s_observed_thread_id));
}

// ---------------------------------------------------------------------------
// (d) observe-during-drain (registry read vs write): a reader thread
// repeatedly dispatches a fixed event (registry READ) while a writer thread
// concurrently registers new async observers (registry WRITE, append-only/
// bump-last). Both threads are JOINED before any TEST_ASSERT.
// ---------------------------------------------------------------------------

#define RACE_WRITER_OBSERVERS 5
#define RACE_READER_ITERATIONS 200

static _Atomic int s_race_hits;

static void race_observer(const bb_lifecycle_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    atomic_fetch_add(&s_race_hits, 1);
}

static void *race_reader_fn(void *arg)
{
    (void)arg;
    bb_lifecycle_event_t evt = { .svc = 0, .inhibits = 0, .version = 1,
                                  .old_state = BB_LIFECYCLE_STOPPED,
                                  .new_state = BB_LIFECYCLE_RUNNING,
                                  .reason = BB_LIFECYCLE_REASON_NONE, ._pad = 0 };
    for (int i = 0; i < RACE_READER_ITERATIONS; i++) {
        bb_lifecycle_async_drain_dispatch_for_test(&evt);
    }
    return NULL;
}

typedef struct {
    int registered_count;
} race_writer_ctx_t;

static void *race_writer_fn(void *arg)
{
    race_writer_ctx_t *ctx = (race_writer_ctx_t *)arg;
    for (int i = 0; i < RACE_WRITER_OBSERVERS; i++) {
        if (bb_lifecycle_observe_async(race_observer, NULL) == BB_OK) {
            ctx->registered_count++;
        }
    }
    return NULL;
}

void test_bb_lifecycle_async_observe_during_drain_no_torn_read(void)
{
    reset_world();
    atomic_store(&s_race_hits, 0);

    pthread_t reader, writer;
    race_writer_ctx_t writer_ctx = { .registered_count = 0 };

    TEST_ASSERT_EQUAL_INT(0, pthread_create(&reader, NULL, race_reader_fn, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&writer, NULL, race_writer_fn, &writer_ctx));

    pthread_join(reader, NULL);
    pthread_join(writer, NULL); // both joined -- safe to assert now

    TEST_ASSERT_EQUAL(RACE_WRITER_OBSERVERS, writer_ctx.registered_count);

    // Deterministic follow-up (single-threaded, post-join): one more
    // dispatch must now see every one of the writer's slots fire exactly
    // once, proving the table ended up in a fully-consistent (non-torn)
    // state after the race, regardless of how the two threads interleaved.
    bb_lifecycle_event_t evt = { .svc = 0, .inhibits = 0, .version = 1,
                                  .old_state = BB_LIFECYCLE_STOPPED,
                                  .new_state = BB_LIFECYCLE_RUNNING,
                                  .reason = BB_LIFECYCLE_REASON_NONE, ._pad = 0 };
    int before = atomic_load(&s_race_hits);
    bb_lifecycle_async_drain_dispatch_for_test(&evt);
    int after = atomic_load(&s_race_hits);
    TEST_ASSERT_EQUAL(RACE_WRITER_OBSERVERS, after - before);
}

// ---------------------------------------------------------------------------
// (g) BB_ERR_UNSUPPORTED when CONFIG_BB_LIFECYCLE_ASYNC=n: this host test
// binary is itself built with BB_LIFECYCLE_ASYNC=1 (platformio.ini), so the
// gate-off runtime path cannot be exercised in THIS binary -- the #else
// branch in bb_lifecycle_async.c (bb_lifecycle_observe_async() always
// returning BB_ERR_UNSUPPORTED, and every other symbol in that branch
// referencing zero bb_bqueue/bb_task types) is exercised by the DEFAULT
// build (CONFIG_BB_LIFECYCLE_ASYNC=n, the shipped default) via `make smoke`
// (env:esp32, no override) -- a compile gate, not a runtime assertion, for
// the disabled path. See bb_lifecycle_async.c's #else block.
// ---------------------------------------------------------------------------
