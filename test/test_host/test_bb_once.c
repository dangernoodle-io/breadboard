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
