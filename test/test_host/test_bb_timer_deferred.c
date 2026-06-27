#include "unity.h"
#include "bb_timer.h"
#include "bb_timer_deferred_test.h"
#include "test_alloc_inject.h"

static volatile int s_work_count = 0;

static void work_fn(void *arg)
{
    (void)arg;
    s_work_count++;
}

static void counter_work_fn(void *arg)
{
    int *count = (int *)arg;
    (*count)++;
}

/* -------------------------------------------------------------------------
 * MODE A: Deferred periodic tests
 * -------------------------------------------------------------------------*/

void test_deferred_periodic_coalesce_skips_second_enqueue(void)
{
    s_work_count = 0;
    bb_timer_host_set_sync_mode(true);

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_deferred_periodic_create(work_fn, NULL, "coal", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    /* First fire: runs work_fn */
    bb_timer_deferred_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_work_count);

    /* Manually mark pending to simulate in-flight work */
    bb_timer_deferred_set_pending_for_test(t, true);

    /* Second fire while pending: should be coalesced (dropped) */
    bb_timer_deferred_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_work_count);

    /* Clear pending */
    bb_timer_deferred_set_pending_for_test(t, false);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_host_set_sync_mode(false);
}

void test_deferred_periodic_no_coalesce_after_drain(void)
{
    s_work_count = 0;
    bb_timer_host_set_sync_mode(false);

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_deferred_periodic_create(work_fn, NULL, "drain", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    /* Set pending manually, then drain */
    bb_timer_deferred_set_pending_for_test(t, true);
    bb_timer_deferred_drain_for_test(t);
    TEST_ASSERT_EQUAL(1, s_work_count);

    /* After drain, pending is false — next fire should work */
    bb_timer_host_set_sync_mode(true);
    bb_timer_deferred_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(2, s_work_count);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_host_set_sync_mode(false);
}

void test_deferred_periodic_fire_before_start_noop(void)
{
    s_work_count = 0;
    bb_timer_host_set_sync_mode(true);

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_deferred_periodic_create(work_fn, NULL, "nostart", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    /* Fire without starting — should be a no-op */
    bb_timer_deferred_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(0, s_work_count);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_host_set_sync_mode(false);
}

void test_deferred_periodic_stop_prevents_fire(void)
{
    s_work_count = 0;
    bb_timer_host_set_sync_mode(true);

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_deferred_periodic_create(work_fn, NULL, "stop", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_stop(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    /* Fire after stop — running is false, should be noop */
    bb_timer_deferred_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(0, s_work_count);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_host_set_sync_mode(false);
}

void test_deferred_periodic_delete_after_stop_safe(void)
{
    bb_timer_host_set_sync_mode(true);

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_deferred_periodic_create(work_fn, NULL, "del", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_stop(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_host_set_sync_mode(false);
}

void test_deferred_periodic_create_malloc_fail(void)
{
    bb_timer_set_malloc_for_test(test_failing_malloc);
    test_alloc_reset();
    test_alloc_fail_at = 0;  /* fail first allocation */

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_deferred_periodic_create(work_fn, NULL, "oom", &t);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);

    bb_timer_set_malloc_for_test(NULL);
}

/* -------------------------------------------------------------------------
 * MODE A: Deferred one-shot tests
 * -------------------------------------------------------------------------*/

void test_deferred_oneshot_fires_once_then_disarmed(void)
{
    s_work_count = 0;

    bb_oneshot_timer_t t;
    bb_err_t err = bb_timer_deferred_oneshot_create(work_fn, NULL, "once", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_deferred_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_work_count);

    /* Second fire: no longer armed, should be a no-op */
    bb_timer_deferred_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_work_count);

    err = bb_timer_oneshot_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_deferred_oneshot_rearm_fires_again(void)
{
    s_work_count = 0;

    bb_oneshot_timer_t t;
    bb_err_t err = bb_timer_deferred_oneshot_create(work_fn, NULL, "rearm", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_deferred_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_work_count);

    /* Re-arm */
    err = bb_timer_oneshot_start(t, 500000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_deferred_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(2, s_work_count);

    err = bb_timer_oneshot_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_deferred_oneshot_create_malloc_fail(void)
{
    bb_timer_set_malloc_for_test(test_failing_malloc);
    test_alloc_reset();
    test_alloc_fail_at = 0;

    bb_oneshot_timer_t t;
    bb_err_t err = bb_timer_deferred_oneshot_create(work_fn, NULL, "oom", &t);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);

    bb_timer_set_malloc_for_test(NULL);
}

/* -------------------------------------------------------------------------
 * MODE B: Worker timer tests
 * -------------------------------------------------------------------------*/

void test_worker_fire_runs_work_fn(void)
{
    int count = 0;

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_worker_periodic_create(counter_work_fn, &count,
                                                   "worker", NULL, &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    /* Synchronously invoke the work function */
    bb_timer_worker_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, count);

    bb_timer_worker_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(2, count);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_worker_create_malloc_fail(void)
{
    bb_timer_set_malloc_for_test(test_failing_malloc);
    test_alloc_reset();
    test_alloc_fail_at = 0;

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_worker_periodic_create(work_fn, NULL, "oom",
                                                   NULL, &t);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);

    bb_timer_set_malloc_for_test(NULL);
}

void test_worker_default_cfg(void)
{
    int count = 0;

    bb_periodic_timer_t t;
    /* Pass NULL cfg — should use defaults */
    bb_err_t err = bb_timer_worker_periodic_create(counter_work_fn, &count,
                                                   "defcfg", NULL, &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_worker_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, count);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_worker_delete_safe(void)
{
    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_worker_periodic_create(work_fn, NULL, "del",
                                                   NULL, &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_stop(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}
