#include "unity.h"
#include "bb_once.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>

// setUp/tearDown: defined in test_main.c (global).

#define BB_ONCE_TEST_THREADS 16

static _Atomic int s_run_count;

static void bb_once_test_incr(void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&s_run_count, 1);
}

static void *bb_once_test_worker(void *arg)
{
    bb_once_t *once = (bb_once_t *)arg;
    bb_once_run(once, bb_once_test_incr, NULL);
    return NULL;
}

// N concurrent threads calling bb_once_run on the same bb_once_t: fn runs
// exactly once, regardless of race timing.
void test_bb_once_run_exactly_once_across_threads(void)
{
    static bb_once_t once = BB_ONCE_INIT;
    atomic_store(&s_run_count, 0);

    pthread_t threads[BB_ONCE_TEST_THREADS];
    for (int i = 0; i < BB_ONCE_TEST_THREADS; i++) {
        int rc = pthread_create(&threads[i], NULL, bb_once_test_worker, &once);
        TEST_ASSERT_EQUAL_INT(0, rc);
    }
    for (int i = 0; i < BB_ONCE_TEST_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_run_count));
}

// A second, later, single-threaded call to bb_once_run on an already-run
// bb_once_t is a no-op: fn is not invoked again.
void test_bb_once_run_second_call_is_noop(void)
{
    bb_once_t once = BB_ONCE_INIT;
    atomic_store(&s_run_count, 0);

    bb_once_run(&once, bb_once_test_incr, NULL);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_run_count));

    bb_once_run(&once, bb_once_test_incr, NULL);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_run_count));
}

// A NULL bb_once_t* is a safe no-op (never dereferences, never crashes).
void test_bb_once_run_null_once_is_safe(void)
{
    atomic_store(&s_run_count, 0);
    bb_once_run(NULL, bb_once_test_incr, NULL);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&s_run_count));
}

// A NULL fn is tolerated: state still transitions to done and callers unblock.
void test_bb_once_run_null_fn_is_safe(void)
{
    bb_once_t once = BB_ONCE_INIT;
    bb_once_run(&once, NULL, NULL);
    // second call should still be a fast no-op, not hang
    bb_once_run(&once, NULL, NULL);
    TEST_PASS();
}

// ---------------------------------------------------------------------------
// A loser that arrives while the winner's fn is still running must actually
// enter the wait loop (host path: sched_yield()) at least once before
// unblocking — deterministic version of the race in
// test_bb_once_run_exactly_once_across_threads, which races threads so fast
// the loser can observe DONE on its very first check and never touch the
// loop body at all.
// ---------------------------------------------------------------------------

static _Atomic bool s_once_winner_started;

static void bb_once_test_slow_incr(void *ctx)
{
    (void)ctx;
    atomic_store(&s_once_winner_started, true);
    usleep(20000); // hold RUNNING long enough for the loser to observe it
    atomic_fetch_add(&s_run_count, 1);
}

static void *bb_once_test_winner_worker(void *arg)
{
    bb_once_t *once = (bb_once_t *)arg;
    bb_once_run(once, bb_once_test_slow_incr, NULL);
    return NULL;
}

static void *bb_once_test_loser_worker(void *arg)
{
    bb_once_t *once = (bb_once_t *)arg;
    // Busy-poll until the winner has entered fn() — guarantees this call
    // observes BB_ONCE_STATE_RUNNING (not DONE) and must loop/yield below.
    while (!atomic_load(&s_once_winner_started)) {
        sched_yield();
    }
    bb_once_run(once, bb_once_test_incr, NULL);
    return NULL;
}

void test_bb_once_run_loser_waits_via_sched_yield(void)
{
    static bb_once_t once = BB_ONCE_INIT;
    atomic_store(&s_run_count, 0);
    atomic_store(&s_once_winner_started, false);

    pthread_t winner, loser;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&winner, NULL, bb_once_test_winner_worker, &once));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&loser, NULL, bb_once_test_loser_worker, &once));

    pthread_join(winner, NULL);
    pthread_join(loser, NULL);

    // Only the winner's slow_incr ran; the loser's incr was skipped (no-op
    // after DONE), so run_count reflects exactly one fn invocation.
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_run_count));
}

// ---------------------------------------------------------------------------
// bb_once_run_fallible() — B1-524 HIGH-1/HIGH-2 fix: unlike bb_once_run(),
// a failed fn() must NOT permanently latch DONE; the guard must reset to
// IDLE so the very next call genuinely retries.
// ---------------------------------------------------------------------------

static _Atomic int  s_fallible_attempt_count;
static _Atomic int  s_fallible_fail_until_attempt; // attempts <= this fail

static bool bb_once_test_fallible_fn(void *ctx)
{
    (void)ctx;
    int n = atomic_fetch_add(&s_fallible_attempt_count, 1) + 1;
    return n > atomic_load(&s_fallible_fail_until_attempt);
}

// First call's fn() fails (simulating a transient allocation failure);
// bb_once_run_fallible() must report false AND reset the guard to IDLE
// (not latch DONE) so a second call genuinely re-attempts fn() and can
// succeed — the exact guarantee the old bb_once_run()-guards-bb_lock_init
// pattern violated.
void test_bb_once_run_fallible_retries_after_transient_failure(void)
{
    bb_once_t once = BB_ONCE_INIT;
    atomic_store(&s_fallible_attempt_count, 0);
    atomic_store(&s_fallible_fail_until_attempt, 1); // attempt 1 fails, attempt 2+ succeeds

    bool first = bb_once_run_fallible(&once, bb_once_test_fallible_fn, NULL);
    TEST_ASSERT_FALSE(first);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_fallible_attempt_count));

    bool second = bb_once_run_fallible(&once, bb_once_test_fallible_fn, NULL);
    TEST_ASSERT_TRUE(second);
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&s_fallible_attempt_count));

    // A third call must be a genuine no-op (latched DONE after the
    // successful second attempt) — fn() must not run a third time.
    bool third = bb_once_run_fallible(&once, bb_once_test_fallible_fn, NULL);
    TEST_ASSERT_TRUE(third);
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&s_fallible_attempt_count));
}

// A successful attempt latches DONE: a later call is a no-op, fn() is not
// re-invoked (mirrors bb_once_run_second_call_is_noop above).
void test_bb_once_run_fallible_second_call_after_success_is_noop(void)
{
    bb_once_t once = BB_ONCE_INIT;
    atomic_store(&s_fallible_attempt_count, 0);
    atomic_store(&s_fallible_fail_until_attempt, 0); // never fails

    TEST_ASSERT_TRUE(bb_once_run_fallible(&once, bb_once_test_fallible_fn, NULL));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_fallible_attempt_count));

    TEST_ASSERT_TRUE(bb_once_run_fallible(&once, bb_once_test_fallible_fn, NULL));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_fallible_attempt_count));
}

// A NULL bb_once_t* is a safe no-op: returns false, never dereferences.
void test_bb_once_run_fallible_null_once_is_safe(void)
{
    atomic_store(&s_fallible_attempt_count, 0);
    TEST_ASSERT_FALSE(bb_once_run_fallible(NULL, bb_once_test_fallible_fn, NULL));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&s_fallible_attempt_count));
}

// A NULL fn is treated as a failed attempt (never a crash): reports false
// and resets to IDLE, not latched DONE — a later call with a real fn can
// still succeed.
void test_bb_once_run_fallible_null_fn_is_safe(void)
{
    bb_once_t once = BB_ONCE_INIT;
    TEST_ASSERT_FALSE(bb_once_run_fallible(&once, NULL, NULL));

    atomic_store(&s_fallible_attempt_count, 0);
    atomic_store(&s_fallible_fail_until_attempt, 0);
    TEST_ASSERT_TRUE(bb_once_run_fallible(&once, bb_once_test_fallible_fn, NULL));
}

// Concurrency: a losing racer that arrives while the winner's fn() is still
// RUNNING, and observes the winner's attempt FAIL (state resets to IDLE
// before the racer's own wait loop re-checks), must return false itself —
// it does not silently retry on the winner's behalf, and it must not hang.
static _Atomic bool s_fallible_winner_started;

static bool bb_once_test_fallible_slow_fail_fn(void *ctx)
{
    (void)ctx;
    atomic_store(&s_fallible_winner_started, true);
    usleep(20000); // hold RUNNING long enough for the loser to observe it
    atomic_fetch_add(&s_fallible_attempt_count, 1);
    return false; // simulated transient failure
}

static void *bb_once_test_fallible_winner_worker(void *arg)
{
    bb_once_t *once = (bb_once_t *)arg;
    bool ok = bb_once_run_fallible(once, bb_once_test_fallible_slow_fail_fn, NULL);
    TEST_ASSERT_FALSE(ok);
    return NULL;
}

static void *bb_once_test_fallible_loser_worker(void *arg)
{
    bb_once_t *once = (bb_once_t *)arg;
    while (!atomic_load(&s_fallible_winner_started)) {
        sched_yield();
    }
    // The winner is RUNNING (about to fail); this call must observe either
    // RUNNING (then loop) or IDLE (post-reset) — never DONE — and must
    // return without invoking fn() itself (fn is the winner's, only the
    // winner's fn() call is allowed to run here).
    bool ok = bb_once_run_fallible(once, NULL, NULL);
    TEST_ASSERT_FALSE(ok);
    return NULL;
}

void test_bb_once_run_fallible_loser_observes_failure_and_returns_false(void)
{
    static bb_once_t once = BB_ONCE_INIT;
    atomic_store(&s_fallible_attempt_count, 0);
    atomic_store(&s_fallible_winner_started, false);

    pthread_t winner, loser;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&winner, NULL, bb_once_test_fallible_winner_worker, &once));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&loser, NULL, bb_once_test_fallible_loser_worker, &once));

    pthread_join(winner, NULL);
    pthread_join(loser, NULL);

    // Only the winner's fn() ran (the loser passed fn=NULL and never won
    // the CAS), and the guard is back to IDLE — a later real attempt can
    // still succeed.
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_fallible_attempt_count));
    atomic_store(&s_fallible_fail_until_attempt, 0);
    atomic_store(&s_fallible_attempt_count, 0);
    TEST_ASSERT_TRUE(bb_once_run_fallible(&once, bb_once_test_fallible_fn, NULL));
}

// ---------------------------------------------------------------------------
// Site-shape tests (B1-524 review finding 5): the real call sites
// (platform/espidf/bb_wifi/bb_wifi.c ping_infra_init, platform/espidf/
// bb_timer/bb_timer.c disp_bootstrap, platform/espidf/bb_mdns/bb_mdns.c
// mdns_init_bootstrap) are ESP-IDF-only — they call FreeRTOS
// xSemaphoreCreateMutex()/xQueueCreate()/bb_task_create() directly and are
// not buildable on host (no host backend exists for those TUs, matching
// this PR's own coverage report). These tests instead mirror each real
// site's EXACT bb_once_run_fallible() body control flow — own NULL
// sentinels, partial-failure cleanup, resume-from-failed-step — against a
// mockable/injectable create hook, proving the retry mechanism each real
// site is built from (not the platform calls themselves, which bb_once_run_
// fallible() never touches).
// ---------------------------------------------------------------------------

// --- bb_wifi_ping's ping_infra_init() shape: 2 independent creates, both
// required; a partial failure frees whichever succeeded so the retry
// starts clean (no leaked handle). ---

static int  s_mock_mutex_handle;  // any non-NULL address stands in for a real handle
static int  s_mock_sem_handle;
static void *s_mock_ping_mutex;
static void *s_mock_ping_done;
static _Atomic bool s_mock_mutex_create_should_fail;
static _Atomic bool s_mock_sem_create_should_fail;
static _Atomic int  s_mock_mutex_create_calls;
static _Atomic int  s_mock_sem_create_calls;
static _Atomic int  s_mock_destroy_calls;

static bool ping_infra_init_shape(void *ctx)
{
    (void)ctx;
    if (!s_mock_ping_mutex) {
        atomic_fetch_add(&s_mock_mutex_create_calls, 1);
        s_mock_ping_mutex = atomic_load(&s_mock_mutex_create_should_fail) ? NULL : &s_mock_mutex_handle;
    }
    if (!s_mock_ping_done) {
        atomic_fetch_add(&s_mock_sem_create_calls, 1);
        s_mock_ping_done = atomic_load(&s_mock_sem_create_should_fail) ? NULL : &s_mock_sem_handle;
    }
    if (s_mock_ping_mutex && s_mock_ping_done) {
        return true;
    }
    if (s_mock_ping_mutex) {
        atomic_fetch_add(&s_mock_destroy_calls, 1);
        s_mock_ping_mutex = NULL;
    }
    if (s_mock_ping_done) {
        atomic_fetch_add(&s_mock_destroy_calls, 1);
        s_mock_ping_done = NULL;
    }
    return false;
}

// Injected first-attempt failure (mock sem create fails, matching
// ping_infra_init's own two-create shape) is retried cleanly on the next
// call: no crash, no permanent latch, sentinels are NULL after the failed
// attempt (partial mutex handle freed, not leaked) and non-NULL after the
// retry succeeds.
void test_bb_once_run_fallible_wifi_ping_infra_shape_retries_on_injected_failure(void)
{
    bb_once_t once = BB_ONCE_INIT;
    s_mock_ping_mutex = NULL;
    s_mock_ping_done = NULL;
    atomic_store(&s_mock_mutex_create_should_fail, false);
    atomic_store(&s_mock_sem_create_should_fail, true); // inject failure
    atomic_store(&s_mock_mutex_create_calls, 0);
    atomic_store(&s_mock_sem_create_calls, 0);
    atomic_store(&s_mock_destroy_calls, 0);

    TEST_ASSERT_FALSE(bb_once_run_fallible(&once, ping_infra_init_shape, NULL));
    TEST_ASSERT_NULL(s_mock_ping_mutex); // freed, not leaked
    TEST_ASSERT_NULL(s_mock_ping_done);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_mock_destroy_calls)); // the mutex that DID create

    atomic_store(&s_mock_sem_create_should_fail, false); // heal
    TEST_ASSERT_TRUE(bb_once_run_fallible(&once, ping_infra_init_shape, NULL));
    TEST_ASSERT_NOT_NULL(s_mock_ping_mutex);
    TEST_ASSERT_NOT_NULL(s_mock_ping_done);
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&s_mock_mutex_create_calls));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&s_mock_sem_create_calls));

    // Latched: a third call must not re-create either resource.
    TEST_ASSERT_TRUE(bb_once_run_fallible(&once, ping_infra_init_shape, NULL));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&s_mock_mutex_create_calls));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&s_mock_sem_create_calls));
}

// --- bb_timer disp_ensure_started()'s disp_bootstrap() shape: queue create
// then task create; a task-create failure tears the queue back down so the
// retry starts clean (mirrors vQueueDelete()+s_disp_queue=NULL on the real
// site). ---

static int  s_mock_queue_handle;
static int  s_mock_task_handle;
static void *s_mock_disp_queue;
static void *s_mock_disp_task;
static _Atomic bool s_mock_queue_create_should_fail;
static _Atomic bool s_mock_task_create_should_fail;
static _Atomic int  s_mock_queue_create_calls;
static _Atomic int  s_mock_task_create_calls;
static _Atomic int  s_mock_queue_delete_calls;

static bool disp_bootstrap_shape(void *ctx)
{
    (void)ctx;
    if (s_mock_disp_queue != NULL) return true;

    atomic_fetch_add(&s_mock_queue_create_calls, 1);
    s_mock_disp_queue = atomic_load(&s_mock_queue_create_should_fail) ? NULL : &s_mock_queue_handle;
    if (s_mock_disp_queue == NULL) return false;

    atomic_fetch_add(&s_mock_task_create_calls, 1);
    s_mock_disp_task = atomic_load(&s_mock_task_create_should_fail) ? NULL : &s_mock_task_handle;
    if (s_mock_disp_task == NULL) {
        atomic_fetch_add(&s_mock_queue_delete_calls, 1);
        s_mock_disp_queue = NULL;
        return false;
    }
    return true;
}

// Injected task-create failure (queue already created) is retried cleanly:
// the queue is torn back down (not leaked) so s_mock_disp_queue == NULL is
// once again the site's own "not done" sentinel for the next call, which
// re-creates both and succeeds.
void test_bb_once_run_fallible_timer_disp_shape_retries_on_injected_failure(void)
{
    bb_once_t once = BB_ONCE_INIT;
    s_mock_disp_queue = NULL;
    s_mock_disp_task = NULL;
    atomic_store(&s_mock_queue_create_should_fail, false);
    atomic_store(&s_mock_task_create_should_fail, true); // inject failure
    atomic_store(&s_mock_queue_create_calls, 0);
    atomic_store(&s_mock_task_create_calls, 0);
    atomic_store(&s_mock_queue_delete_calls, 0);

    TEST_ASSERT_FALSE(bb_once_run_fallible(&once, disp_bootstrap_shape, NULL));
    TEST_ASSERT_NULL(s_mock_disp_queue);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_mock_queue_delete_calls));

    atomic_store(&s_mock_task_create_should_fail, false); // heal
    TEST_ASSERT_TRUE(bb_once_run_fallible(&once, disp_bootstrap_shape, NULL));
    TEST_ASSERT_NOT_NULL(s_mock_disp_queue);
    TEST_ASSERT_NOT_NULL(s_mock_disp_task);
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&s_mock_queue_create_calls));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&s_mock_task_create_calls));
}

// --- bb_mdns_init()'s mdns_init_bootstrap() shape: N independent
// resources, EACH its own "if (!s_X)" sentinel (no rollback on failure, by
// design — matches the real bb_mdns_init_locked() cascade unchanged). A
// mid-cascade failure must leave every earlier-created resource intact and
// only retry the resources that never got created. ---

static int  s_mock_res_a_h, s_mock_res_b_h, s_mock_res_c_h;
static void *s_mock_res_a, *s_mock_res_b, *s_mock_res_c;
static _Atomic bool s_mock_res_b_should_fail;
static _Atomic int  s_mock_res_a_create_calls, s_mock_res_b_create_calls, s_mock_res_c_create_calls;

static bool mdns_init_bootstrap_shape(void *ctx)
{
    (void)ctx;
    if (!s_mock_res_a) {
        atomic_fetch_add(&s_mock_res_a_create_calls, 1);
        s_mock_res_a = &s_mock_res_a_h;
    }
    if (!s_mock_res_b) {
        atomic_fetch_add(&s_mock_res_b_create_calls, 1);
        s_mock_res_b = atomic_load(&s_mock_res_b_should_fail) ? NULL : &s_mock_res_b_h;
    }
    if (!s_mock_res_c) {
        atomic_fetch_add(&s_mock_res_c_create_calls, 1);
        s_mock_res_c = &s_mock_res_c_h;
    }
    return s_mock_res_a && s_mock_res_b && s_mock_res_c;
}

// Injected mid-cascade failure (resource B) on the first attempt: resource
// A (created before the failure) and resource C (created after B, since
// the real bb_mdns_init_locked() cascade does not early-return on a single
// step's failure) are NOT re-created on the retry — only B's own sentinel
// was left NULL, so the retry resumes from exactly that step.
void test_bb_once_run_fallible_mdns_cascade_shape_resumes_from_failed_step(void)
{
    bb_once_t once = BB_ONCE_INIT;
    s_mock_res_a = s_mock_res_b = s_mock_res_c = NULL;
    atomic_store(&s_mock_res_b_should_fail, true); // inject failure
    atomic_store(&s_mock_res_a_create_calls, 0);
    atomic_store(&s_mock_res_b_create_calls, 0);
    atomic_store(&s_mock_res_c_create_calls, 0);

    TEST_ASSERT_FALSE(bb_once_run_fallible(&once, mdns_init_bootstrap_shape, NULL));
    TEST_ASSERT_NOT_NULL(s_mock_res_a);
    TEST_ASSERT_NULL(s_mock_res_b);
    TEST_ASSERT_NOT_NULL(s_mock_res_c);

    atomic_store(&s_mock_res_b_should_fail, false); // heal
    TEST_ASSERT_TRUE(bb_once_run_fallible(&once, mdns_init_bootstrap_shape, NULL));
    TEST_ASSERT_NOT_NULL(s_mock_res_b);

    // A and C were never re-created (their own sentinels were already
    // non-NULL going into the retry) — only B's create ran a second time.
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_mock_res_a_create_calls));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&s_mock_res_b_create_calls));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_mock_res_c_create_calls));
}

// --- Arity-guard regression test (B1-524 review HIGH/LOW-1/LOW-4): the
// real bb_mdns.c bug this reproduces was mdns_init_bootstrap()'s success
// predicate OMITTING s_flush_timer from its `&&` chain even though
// flush_timer_ensure_created() (a best-effort, log-only-on-failure create,
// like the D resource below) is created in the SAME cascade — a transient
// flush-timer failure alongside every other resource succeeding still
// returned true and latched DONE permanently, silently disabling
// coalescing forever. This test proves the property a correct predicate
// must have (every created resource is included) by contrasting a
// deliberately BUGGY predicate (mirrors the omission) against the FIXED
// one (mirrors the real file's current predicate, which includes all 4/10
// resources) against the identical cascade body. ---

static void *s_mock_res_d; // stands in for s_flush_timer: best-effort, log-only on failure
static _Atomic bool s_mock_res_d_should_fail;

static void mdns_cascade_with_d(void)
{
    if (!s_mock_res_a) s_mock_res_a = &s_mock_res_a_h;
    if (!s_mock_res_b) s_mock_res_b = &s_mock_res_b_h;
    if (!s_mock_res_c) s_mock_res_c = &s_mock_res_c_h;
    if (!s_mock_res_d) {
        // "best-effort" create: on failure, just leaves s_mock_res_d NULL
        // and would only log a warning in the real flush_timer_ensure_
        // created() -- never surfaces an error to its caller.
        s_mock_res_d = atomic_load(&s_mock_res_d_should_fail) ? NULL : (void *)1;
    }
}

// BUGGY shape: mirrors the pre-fix bb_mdns.c predicate that omitted
// s_flush_timer/D from its success check.
static bool mdns_init_bootstrap_shape_buggy_omits_d(void *ctx)
{
    (void)ctx;
    mdns_cascade_with_d();
    return s_mock_res_a && s_mock_res_b && s_mock_res_c; // BUG: D omitted
}

// FIXED shape: mirrors the current bb_mdns.c predicate, which includes
// every resource the cascade creates.
static bool mdns_init_bootstrap_shape_fixed_includes_d(void *ctx)
{
    (void)ctx;
    mdns_cascade_with_d();
    return s_mock_res_a && s_mock_res_b && s_mock_res_c && s_mock_res_d;
}

void test_bb_once_run_fallible_mdns_cascade_shape_predicate_must_cover_every_resource(void)
{
    // Buggy predicate: D fails, A/B/C succeed -- the omission makes this
    // report success and latch DONE anyway (the exact regression).
    {
        bb_once_t once = BB_ONCE_INIT;
        s_mock_res_a = s_mock_res_b = s_mock_res_c = s_mock_res_d = NULL;
        atomic_store(&s_mock_res_d_should_fail, true);

        bool ok = bb_once_run_fallible(&once, mdns_init_bootstrap_shape_buggy_omits_d, NULL);
        TEST_ASSERT_TRUE(ok);       // demonstrates the bug: reports success...
        TEST_ASSERT_NULL(s_mock_res_d); // ...even though D never got created.

        // Latched: even after D would now succeed, a retry never happens
        // because the guard is already DONE -- D stays NULL forever.
        atomic_store(&s_mock_res_d_should_fail, false);
        TEST_ASSERT_TRUE(bb_once_run_fallible(&once, mdns_init_bootstrap_shape_buggy_omits_d, NULL));
        TEST_ASSERT_NULL(s_mock_res_d);
    }

    // Fixed predicate: identical injected D failure, but D is included in
    // the `&&` chain -- reports failure, guard resets to IDLE, and a
    // healed retry succeeds and actually creates D.
    {
        bb_once_t once = BB_ONCE_INIT;
        s_mock_res_a = s_mock_res_b = s_mock_res_c = s_mock_res_d = NULL;
        atomic_store(&s_mock_res_d_should_fail, true);

        bool first = bb_once_run_fallible(&once, mdns_init_bootstrap_shape_fixed_includes_d, NULL);
        TEST_ASSERT_FALSE(first);
        TEST_ASSERT_NULL(s_mock_res_d);

        atomic_store(&s_mock_res_d_should_fail, false); // heal
        bool second = bb_once_run_fallible(&once, mdns_init_bootstrap_shape_fixed_includes_d, NULL);
        TEST_ASSERT_TRUE(second);
        TEST_ASSERT_NOT_NULL(s_mock_res_d);
    }
}
