#include "unity.h"
#include "bb_lock.h"
#include "bb_clock.h"
#include <pthread.h>
#include <sched.h>
#include <string.h>

// setUp/tearDown: defined in test_main.c (global).
//
// Sequencing convention (B1-822): every multi-thread test below uses a
// PREDICATE-UNDER-THE-MUTEX release gate, never a fixed sleep. A waiter sets
// its own `registered[i]` flag while holding `lock`, immediately before
// calling bb_lock_cond_wait() -- since the waiter never releases `lock`
// between the two, the main thread can only observe `registered[i] == 1`
// (checked itself under `lock`, in a sched_yield() spin -- no sleep) once
// that waiter's bb_lock_cond_wait() call has performed its internal atomic
// unlock-and-register, which by contract (both backends, see bb_lock.h) only
// happens once the waiter is safely enqueued. This makes every subsequent
// signal()/broadcast() call provably non-racing against a not-yet-registered
// waiter.

// ---------------------------------------------------------------------------
// Shared waiter thread fixture
// ---------------------------------------------------------------------------

typedef struct {
    bb_lock_t *lock;
    bb_lock_cond_t *cond;
    int *registered;     // optional; NULL if this test doesn't use the gate
    int index;
    uint32_t timeout_ms;
    bb_err_t result;
    bb_err_t unlock_result;
    uint64_t elapsed_us;
} bb_lock_cond_test_ctx_t;

static void *bb_lock_cond_test_waiter_fn(void *arg)
{
    bb_lock_cond_test_ctx_t *ctx = (bb_lock_cond_test_ctx_t *)arg;
    bb_lock_lock(ctx->lock);
    if (ctx->registered) {
        ctx->registered[ctx->index] = 1;
    }
    uint64_t t0 = bb_clock_now_us();
    ctx->result = bb_lock_cond_wait(ctx->cond, ctx->lock, ctx->timeout_ms);
    ctx->elapsed_us = bb_clock_now_us() - t0;
    ctx->unlock_result = bb_lock_unlock(ctx->lock);
    return NULL;
}

static bool bb_lock_cond_test_all_registered(bb_lock_t *lock, int *registered, int n)
{
    bb_lock_lock(lock);
    bool all = true;
    for (int i = 0; i < n; i++) {
        if (!registered[i]) {
            all = false;
            break;
        }
    }
    bb_lock_unlock(lock);
    return all;
}

// ---------------------------------------------------------------------------
// bb_lock_cond_init / destroy — sanity
// ---------------------------------------------------------------------------

void test_bb_lock_cond_init_and_destroy_basic(void)
{
    bb_lock_cond_t cond;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_init(&cond));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_destroy(&cond));
}

void test_bb_lock_cond_init_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_cond_init(NULL));
}

void test_bb_lock_cond_destroy_never_initialized_is_noop_ok(void)
{
    bb_lock_cond_t cond;
    memset(&cond, 0, sizeof(cond));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_destroy(&cond));
}

void test_bb_lock_cond_destroy_twice_returns_invalid_state(void)
{
    bb_lock_cond_t cond;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_init(&cond));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_destroy(&cond));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_lock_cond_destroy(&cond));
}

// ---------------------------------------------------------------------------
// NULL-arg guards on every entry point.
// ---------------------------------------------------------------------------

void test_bb_lock_cond_destroy_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_cond_destroy(NULL));
}

void test_bb_lock_cond_wait_null_cond_returns_invalid_arg(void)
{
    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_cond_wait(NULL, &lock, 10));
    bb_lock_destroy(&lock);
}

void test_bb_lock_cond_wait_null_lock_returns_invalid_arg(void)
{
    bb_lock_cond_t cond;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_init(&cond));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_cond_wait(&cond, NULL, 10));
    bb_lock_cond_destroy(&cond);
}

void test_bb_lock_cond_signal_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_cond_signal(NULL));
}

void test_bb_lock_cond_broadcast_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_cond_broadcast(NULL));
}

// ---------------------------------------------------------------------------
// Real block/wake: a waiter genuinely blocks in wait() (BB_LOCK_COND_WAIT_FOREVER,
// exercising the no-timeout path), a second thread signals only once the
// release gate confirms registration, and the waiter returns BB_OK having
// actually blocked. Lower-bound-only assertion on elapsed time (never an
// upper bound) to avoid CI-jitter flakiness.
// ---------------------------------------------------------------------------

void test_bb_lock_cond_wait_real_block_then_signal_wakes(void)
{
    bb_lock_t lock;
    bb_lock_cond_t cond;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_init(&cond));

    int registered[1] = { 0 };
    bb_lock_cond_test_ctx_t ctx = {
        .lock = &lock, .cond = &cond, .registered = registered, .index = 0,
        .timeout_ms = BB_LOCK_COND_WAIT_FOREVER,
    };
    pthread_t waiter;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&waiter, NULL, bb_lock_cond_test_waiter_fn, &ctx));

    while (!bb_lock_cond_test_all_registered(&lock, registered, 1)) {
        sched_yield();
    }

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_signal(&cond));

    pthread_join(waiter, NULL);

    TEST_ASSERT_EQUAL(BB_OK, ctx.result);
    TEST_ASSERT_EQUAL(BB_OK, ctx.unlock_result);
    // Real block/wake occurred -- lower bound only.
    TEST_ASSERT_GREATER_THAN_UINT64(0, ctx.elapsed_us);

    bb_lock_cond_destroy(&cond);
    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// Broadcast wakes ALL waiters -- the critical test: a signal()-only
// implementation would leave N-1 of these blocked until their own timeout.
// ---------------------------------------------------------------------------

#define BB_LOCK_COND_TEST_BROADCAST_N 3

void test_bb_lock_cond_broadcast_wakes_all_waiters(void)
{
    bb_lock_t lock;
    bb_lock_cond_t cond;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_init(&cond));

    int registered[BB_LOCK_COND_TEST_BROADCAST_N] = { 0 };
    bb_lock_cond_test_ctx_t ctxs[BB_LOCK_COND_TEST_BROADCAST_N];
    pthread_t threads[BB_LOCK_COND_TEST_BROADCAST_N];

    for (int i = 0; i < BB_LOCK_COND_TEST_BROADCAST_N; i++) {
        ctxs[i] = (bb_lock_cond_test_ctx_t){
            .lock = &lock, .cond = &cond, .registered = registered, .index = i,
            .timeout_ms = 5000, .result = BB_ERR_INVALID_STATE,
        };
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, bb_lock_cond_test_waiter_fn, &ctxs[i]));
    }

    while (!bb_lock_cond_test_all_registered(&lock, registered, BB_LOCK_COND_TEST_BROADCAST_N)) {
        sched_yield();
    }

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_broadcast(&cond));

    for (int i = 0; i < BB_LOCK_COND_TEST_BROADCAST_N; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT_EQUAL(BB_OK, ctxs[i].result);
    }

    bb_lock_cond_destroy(&cond);
    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// Signal wakes exactly one -- 2 waiters, 1 signal: exactly one returns BB_OK,
// the other stays blocked until its own (short, bounded) timeout.
// ---------------------------------------------------------------------------

void test_bb_lock_cond_signal_wakes_exactly_one(void)
{
    bb_lock_t lock;
    bb_lock_cond_t cond;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_init(&cond));

    int registered[2] = { 0 };
    bb_lock_cond_test_ctx_t ctxs[2];
    pthread_t threads[2];

    for (int i = 0; i < 2; i++) {
        ctxs[i] = (bb_lock_cond_test_ctx_t){
            .lock = &lock, .cond = &cond, .registered = registered, .index = i,
            .timeout_ms = 300, .result = BB_ERR_INVALID_STATE,
        };
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, bb_lock_cond_test_waiter_fn, &ctxs[i]));
    }

    while (!bb_lock_cond_test_all_registered(&lock, registered, 2)) {
        sched_yield();
    }

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_signal(&cond));

    int ok_count = 0;
    int timeout_count = 0;
    for (int i = 0; i < 2; i++) {
        pthread_join(threads[i], NULL);
        if (ctxs[i].result == BB_OK) {
            ok_count++;
        } else if (ctxs[i].result == BB_ERR_TIMEOUT) {
            timeout_count++;
        }
    }

    TEST_ASSERT_EQUAL_INT(1, ok_count);
    TEST_ASSERT_EQUAL_INT(1, timeout_count);

    bb_lock_cond_destroy(&cond);
    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// Two back-to-back signals wake TWO DISTINCT waiters -- the integration-level
// regression for the ESP-IDF backend's B1-822 review MED-1 missed-wakeup fix
// (signal() must dequeue the head waiter, not merely peek it; see
// test_bb_lock_cond_waiterlist.c for the algorithm-level non-vacuity proof
// against the actual buggy-vs-fixed list-splice logic). On this host
// pthread_cond_t backend both signals correctly wake distinct waiters
// regardless (POSIX guarantees it), so this test is a regression guard
// against any future backend reintroducing the give-without-dequeue bug,
// not itself a reproduction of the ESP-IDF bug.
// ---------------------------------------------------------------------------

void test_bb_lock_cond_two_back_to_back_signals_wake_two_distinct_waiters(void)
{
    bb_lock_t lock;
    bb_lock_cond_t cond;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_init(&cond));

    int registered[2] = { 0 };
    bb_lock_cond_test_ctx_t ctxs[2];
    pthread_t threads[2];

    for (int i = 0; i < 2; i++) {
        ctxs[i] = (bb_lock_cond_test_ctx_t){
            .lock = &lock, .cond = &cond, .registered = registered, .index = i,
            .timeout_ms = 5000, .result = BB_ERR_INVALID_STATE,
        };
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, bb_lock_cond_test_waiter_fn, &ctxs[i]));
    }

    while (!bb_lock_cond_test_all_registered(&lock, registered, 2)) {
        sched_yield();
    }

    // No scheduling gap between the two signal() calls -- both must land
    // before either waiter has a chance to run its own unlink().
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_signal(&cond));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_signal(&cond));

    for (int i = 0; i < 2; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT_EQUAL(BB_OK, ctxs[i].result);
    }

    bb_lock_cond_destroy(&cond);
    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// Timeout expiry: wait() with a short bounded timeout and no signaller
// returns BB_ERR_TIMEOUT at-or-after the requested interval, with the lock
// re-acquired (proven by the caller's own subsequent successful unlock()).
// ---------------------------------------------------------------------------

void test_bb_lock_cond_wait_timeout_expiry_returns_timeout_and_reacquires_lock(void)
{
    bb_lock_t lock;
    bb_lock_cond_t cond;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_init(&cond));

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));
    uint64_t t0 = bb_clock_now_us();
    bb_err_t rc = bb_lock_cond_wait(&cond, &lock, 20);
    uint64_t elapsed_us = bb_clock_now_us() - t0;

    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, rc);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(20000, elapsed_us);
    // Lock re-acquired -- an unlock by a thread NOT holding it would fail.
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));

    bb_lock_cond_destroy(&cond);
    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// The lock is genuinely released while a waiter blocks (a second thread can
// acquire it) and re-acquired before wait() returns (proven by the waiter's
// own final unlock() succeeding).
// ---------------------------------------------------------------------------

void test_bb_lock_cond_wait_releases_lock_while_blocked(void)
{
    bb_lock_t lock;
    bb_lock_cond_t cond;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_init(&cond));

    int registered[1] = { 0 };
    bb_lock_cond_test_ctx_t ctx = {
        .lock = &lock, .cond = &cond, .registered = registered, .index = 0,
        .timeout_ms = 5000,
    };
    pthread_t waiter;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&waiter, NULL, bb_lock_cond_test_waiter_fn, &ctx));

    while (!bb_lock_cond_test_all_registered(&lock, registered, 1)) {
        sched_yield();
    }

    // The lock must be genuinely released while the waiter blocks -- proven
    // by this (second) thread successfully acquiring it.
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_trylock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_signal(&cond));

    pthread_join(waiter, NULL);

    TEST_ASSERT_EQUAL(BB_OK, ctx.result);
    // wait() re-acquired the lock before returning.
    TEST_ASSERT_EQUAL(BB_OK, ctx.unlock_result);

    bb_lock_cond_destroy(&cond);
    bb_lock_destroy(&lock);
}
