#include "unity.h"
#include "bb_timer.h"
#include "bb_timer_oneshot_test.h"

static volatile int s_fire_count = 0;

static void fire_cb(void *arg)
{
    (void)arg;
    s_fire_count++;
}

static void counter_cb(void *arg)
{
    int *count = (int *)arg;
    (*count)++;
}

void test_bb_timer_oneshot_create_null_cb_returns_err(void)
{
    bb_oneshot_timer_t t;
    bb_err_t err = bb_timer_oneshot_create(NULL, NULL, "x", &t);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_bb_timer_oneshot_create_null_out_returns_err(void)
{
    bb_err_t err = bb_timer_oneshot_create(fire_cb, NULL, "x", NULL);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_bb_timer_oneshot_fires_once(void)
{
    s_fire_count = 0;

    bb_oneshot_timer_t t;
    bb_err_t err = bb_timer_oneshot_create(fire_cb, NULL, "once", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_fire_count);

    // Second fire_for_test after firing is a no-op (not re-armed)
    bb_timer_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_fire_count);

    err = bb_timer_oneshot_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_oneshot_rearm_fires_again(void)
{
    s_fire_count = 0;

    bb_oneshot_timer_t t;
    bb_err_t err = bb_timer_oneshot_create(fire_cb, NULL, "rearm", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_fire_count);

    // Re-arm and fire again
    err = bb_timer_oneshot_start(t, 500000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(2, s_fire_count);

    err = bb_timer_oneshot_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_oneshot_stop_prevents_fire(void)
{
    s_fire_count = 0;

    bb_oneshot_timer_t t;
    bb_err_t err = bb_timer_oneshot_create(fire_cb, NULL, "stop", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_stop(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(0, s_fire_count);  // No invocation after stop

    err = bb_timer_oneshot_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_oneshot_delete_frees(void)
{
    bb_oneshot_timer_t t;
    bb_err_t err = bb_timer_oneshot_create(fire_cb, NULL, "del", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_stop(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_oneshot_arg_passed_to_cb(void)
{
    int count = 0;

    bb_oneshot_timer_t t;
    bb_err_t err = bb_timer_oneshot_create(counter_cb, &count, "arg", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_oneshot_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_oneshot_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, count);

    err = bb_timer_oneshot_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}
