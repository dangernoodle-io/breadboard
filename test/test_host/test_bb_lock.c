#include "unity.h"
#include "bb_lock.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

// setUp/tearDown: defined in test_main.c (global). Each test below restores
// bb_lock_stats_set_enabled(true) on entry/exit since the runtime flag is
// process-global state shared with any other bb_lock consumer.

// ---------------------------------------------------------------------------
// bb_lock_init / destroy — sanity
// ---------------------------------------------------------------------------

void test_bb_lock_init_and_destroy_basic(void)
{
    bb_lock_config_t cfg = { .name = "test", .category = "cat" };
    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(&cfg, &lock));
    TEST_ASSERT_EQUAL_STRING("test", lock.name);
    TEST_ASSERT_EQUAL_STRING("cat", lock.category);
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_destroy(&lock));
}

void test_bb_lock_init_null_out_returns_invalid_arg(void)
{
    bb_lock_config_t cfg = { .name = "test", .category = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_init(&cfg, NULL));
}

// A non-NULL cfg with NULL name/category fields exercises the false branch
// of "if (cfg->name)" / "if (cfg->category)" — distinct from the cfg==NULL
// path (test_bb_lock_stats_*, which pass NULL for cfg entirely) and the
// both-set path (test_bb_lock_init_and_destroy_basic).
void test_bb_lock_init_cfg_with_null_name_and_category_leaves_defaults(void)
{
    bb_lock_config_t cfg = { .name = NULL, .category = NULL };
    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(&cfg, &lock));
    TEST_ASSERT_EQUAL_STRING("", lock.name);
    TEST_ASSERT_EQUAL_STRING("", lock.category);
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_destroy(&lock));
}

// ---------------------------------------------------------------------------
// bb_lock_destroy on a never-init'd (zero-initialized) handle stays a no-op
// per the header contract — distinct from the double-destroy case below.
// ---------------------------------------------------------------------------

void test_bb_lock_destroy_never_initialized_is_noop_ok(void)
{
    bb_lock_t lock;
    memset(&lock, 0, sizeof(lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_destroy(&lock));
}

// ---------------------------------------------------------------------------
// bb_lock_destroy on an already-destroyed handle returns BB_ERR_INVALID_STATE
// and must NOT re-invoke the backend destroy primitive (pthread_mutex_destroy
// / vSemaphoreDelete is UB/corruption on an already-freed primitive).
// ---------------------------------------------------------------------------

void test_bb_lock_destroy_twice_returns_invalid_state(void)
{
    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_destroy(&lock));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_lock_destroy(&lock));
}

// ---------------------------------------------------------------------------
// bb_lock_destroy while another thread holds the lock returns
// BB_ERR_INVALID_STATE without destroying the primitive — the lock is still
// usable afterward (destroy did not run) once released.
// ---------------------------------------------------------------------------

typedef struct {
    bb_lock_t *lock;
    useconds_t hold_us;
} bb_lock_test_destroy_holder_ctx_t;

static void *bb_lock_test_destroy_holder(void *arg)
{
    bb_lock_test_destroy_holder_ctx_t *ctx = (bb_lock_test_destroy_holder_ctx_t *)arg;
    bb_lock_lock(ctx->lock);
    usleep(ctx->hold_us);
    bb_lock_unlock(ctx->lock);
    return NULL;
}

void test_bb_lock_destroy_while_held_returns_invalid_state(void)
{
    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    bb_lock_test_destroy_holder_ctx_t ctx = { .lock = &lock, .hold_us = 20000 };
    pthread_t holder;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&holder, NULL, bb_lock_test_destroy_holder, &ctx));

    // Give the holder a head start so it is guaranteed to have acquired the
    // lock before we try to destroy it.
    usleep(1000);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_lock_destroy(&lock));

    pthread_join(holder, NULL);

    // The lock was never actually destroyed — still fully usable, and a
    // real destroy now succeeds.
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_destroy(&lock));
}

// ---------------------------------------------------------------------------
// Stats disabled at runtime: zeroed struct after real traffic. This is
// functionally identical to the BB_LOCK_STATS_ENABLE=0 compile-time path —
// both converge on bb_lock_get_stats() returning an all-zero struct — so a
// single native binary (built with BB_LOCK_STATS_ENABLE=1, per
// platformio.ini) exercises both behaviors: bb_mem's own stats tests follow
// this same runtime-off convention.
// ---------------------------------------------------------------------------

void test_bb_lock_stats_disabled_returns_zeroed_after_traffic(void)
{
    bb_lock_stats_set_enabled(false);

    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    for (int i = 0; i < 5; i++) {
        bb_lock_lock(&lock);
        bb_lock_unlock(&lock);
    }

    bb_lock_stats_t s;
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.acquisition_count);
    TEST_ASSERT_EQUAL_UINT32(0, s.contention_count);
    TEST_ASSERT_EQUAL_UINT64(0, s.wait_time_total_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.hold_time_total_us);

    bb_lock_destroy(&lock);
    bb_lock_stats_set_enabled(true);
}

// ---------------------------------------------------------------------------
// Stats enabled, uncontended: acquisition_count==N, contention_count==0,
// hold_time_total_us > 0.
// ---------------------------------------------------------------------------

void test_bb_lock_stats_enabled_uncontended(void)
{
    bb_lock_stats_set_enabled(true);
    TEST_ASSERT_TRUE(bb_lock_stats_enabled());

    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    const int n = 5;
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));
        TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));
    }

    bb_lock_stats_t s;
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT32(n, s.acquisition_count);
    TEST_ASSERT_EQUAL_UINT32(0, s.contention_count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(0, s.hold_time_total_us);

    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// Stats enabled, contended: a second thread holds the lock ~5ms while the
// main thread blocks on bb_lock_lock — contention_count==1, wait roughly
// matches the hold duration.
// ---------------------------------------------------------------------------

typedef struct {
    bb_lock_t *lock;
    useconds_t hold_us;
} bb_lock_test_holder_ctx_t;

static void *bb_lock_test_holder(void *arg)
{
    bb_lock_test_holder_ctx_t *ctx = (bb_lock_test_holder_ctx_t *)arg;
    bb_lock_lock(ctx->lock);
    usleep(ctx->hold_us);
    bb_lock_unlock(ctx->lock);
    return NULL;
}

void test_bb_lock_stats_enabled_contended(void)
{
    bb_lock_stats_set_enabled(true);

    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    bb_lock_test_holder_ctx_t ctx = { .lock = &lock, .hold_us = 5000 };
    pthread_t holder;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&holder, NULL, bb_lock_test_holder, &ctx));

    // Give the holder thread a head start so it is guaranteed to have
    // acquired the lock before we try.
    usleep(1000);

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));

    pthread_join(holder, NULL);

    bb_lock_stats_t s;
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT32(2, s.acquisition_count); // holder + main
    TEST_ASSERT_EQUAL_UINT32(1, s.contention_count);
    // Waited roughly the remaining hold duration (~4ms); allow generous slop
    // for scheduler jitter on a loaded CI host.
    TEST_ASSERT_GREATER_THAN_UINT64(0, s.wait_time_total_us);
    TEST_ASSERT_GREATER_THAN_UINT64(0, s.wait_time_max_us);

    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// bb_lock_reset_stats zeroes all counters.
// ---------------------------------------------------------------------------

void test_bb_lock_reset_stats_zeroes(void)
{
    bb_lock_stats_set_enabled(true);

    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    bb_lock_lock(&lock);
    bb_lock_unlock(&lock);

    bb_lock_stats_t s;
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.acquisition_count);

    bb_lock_reset_stats(&lock);
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.acquisition_count);
    TEST_ASSERT_EQUAL_UINT32(0, s.contention_count);
    TEST_ASSERT_EQUAL_UINT64(0, s.wait_time_total_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.wait_time_max_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.hold_time_total_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.hold_time_max_us);

    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// bb_lock_trylock on an already-held lock returns BB_ERR_TIMEOUT and does
// NOT bump contention_count (trylock-fail is not contention).
// ---------------------------------------------------------------------------

void test_bb_lock_trylock_on_held_returns_timeout_no_contention(void)
{
    bb_lock_stats_set_enabled(true);

    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_lock_trylock(&lock));

    bb_lock_stats_t s;
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.contention_count);
    TEST_ASSERT_EQUAL_UINT32(1, s.acquisition_count); // only the initial lock()

    bb_lock_unlock(&lock);
    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// bb_lock_held_since_us must be cleared on every unlock() regardless of the
// current runtime stats flag — otherwise a stale timestamp left by an
// un-instrumented unlock (flag OFF) survives into a later un-instrumented
// acquire, and toggling the flag ON before *that* acquire's unlock computes
// a garbage hold_us from the ancient timestamp.
// ---------------------------------------------------------------------------

void test_bb_lock_held_since_cleared_across_runtime_toggle_mid_hold(void)
{
    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    // (a) stats ON: acquire is instrumented — held_since_us set to a real
    // timestamp.
    bb_lock_stats_set_enabled(true);
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));

    // (b) toggle OFF before unlock. Pre-fix, an un-instrumented unlock
    // skipped the release-side bookkeeping entirely, leaving held_since_us
    // stuck at (a)'s timestamp.
    bb_lock_stats_set_enabled(false);
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));

    // (a)-(b)'s unlock legitimately records a real (instrumented-acquire)
    // hold time here — its magnitude is real wall-clock elapsed time and is
    // NOT deterministic (a loaded CI runner can observe >=1us where a quiet
    // box observes 0), so asserting an exact value on it would itself be a
    // race. Reset counters now so the assertions below check only the
    // invariant this test exists to cover: whether the (c)-(d) uninstrumented
    // acquire leaks a bogus hold time, not the real timing of (a)-(b).
    bb_lock_reset_stats(&lock);

    // Let real wall-clock time pass so a stale held_since_us (if the bug
    // were present) would yield an obviously bogus multi-ms hold_us below.
    usleep(5000);

    // (c) stats still OFF: acquire again — un-instrumented, so this lock()
    // never touches held_since_us either way.
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));

    // (d) toggle ON mid-hold, then unlock — the exact scenario the fix
    // targets: an un-instrumented acquire whose unlock now runs with stats
    // enabled.
    bb_lock_stats_set_enabled(true);
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));

    bb_lock_stats_t s;
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT64(0, s.hold_time_total_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.hold_time_max_us);

    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// NULL-arg guards on every entry point that takes a bb_lock_t* — each returns
// BB_ERR_INVALID_ARG (or is a safe no-op) without dereferencing.
// ---------------------------------------------------------------------------

void test_bb_lock_destroy_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_destroy(NULL));
}

void test_bb_lock_lock_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_lock(NULL));
}

void test_bb_lock_trylock_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_trylock(NULL));
}

void test_bb_lock_unlock_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_unlock(NULL));
}

void test_bb_lock_reset_stats_null_is_noop(void)
{
    // No crash, no-op — nothing to assert beyond "did not dereference NULL".
    bb_lock_reset_stats(NULL);
    TEST_PASS();
}

// ---------------------------------------------------------------------------
// bb_lock_trylock success path with stats enabled: the uncontended acquire
// still records acquisition_count via bb_lock_record_acquired (distinct from
// bb_lock_lock's own uncontended-fastpath call to the same helper).
// ---------------------------------------------------------------------------

void test_bb_lock_trylock_success_with_stats_enabled_records_acquired(void)
{
    bb_lock_stats_set_enabled(true);

    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_trylock(&lock));

    bb_lock_stats_t s;
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.acquisition_count);
    TEST_ASSERT_EQUAL_UINT32(0, s.contention_count);

    bb_lock_unlock(&lock);
    bb_lock_destroy(&lock);
}

// ---------------------------------------------------------------------------
// bb_lock_trylock success with the compile gate on but the runtime flag OFF
// — the false branch of trylock's own runtime-flag check, distinct from
// bb_lock_lock's identical-shaped check and from the stats-enabled trylock
// test above.
// ---------------------------------------------------------------------------

void test_bb_lock_trylock_success_with_stats_runtime_disabled(void)
{
    bb_lock_stats_set_enabled(false);

    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_trylock(&lock));

    bb_lock_stats_t s;
    bb_lock_get_stats(&lock, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.acquisition_count);

    bb_lock_unlock(&lock);
    bb_lock_destroy(&lock);
    bb_lock_stats_set_enabled(true);
}

// ---------------------------------------------------------------------------
// bb_lock_get_stats: NULL out is a safe no-op; NULL lock with a real out
// zeroes the caller's struct (the "lock &&" short-circuit branch).
// ---------------------------------------------------------------------------

void test_bb_lock_get_stats_null_out_is_noop(void)
{
    bb_lock_t lock;
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_init(NULL, &lock));
    bb_lock_get_stats(&lock, NULL); // no crash
    bb_lock_destroy(&lock);
}

void test_bb_lock_get_stats_null_lock_zeroes_out(void)
{
    bb_lock_stats_t s;
    memset(&s, 0xAA, sizeof(s));
    bb_lock_get_stats(NULL, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.acquisition_count);
    TEST_ASSERT_EQUAL_UINT32(0, s.contention_count);
    TEST_ASSERT_EQUAL_UINT64(0, s.wait_time_total_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.wait_time_max_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.hold_time_total_us);
    TEST_ASSERT_EQUAL_UINT64(0, s.hold_time_max_us);
}
