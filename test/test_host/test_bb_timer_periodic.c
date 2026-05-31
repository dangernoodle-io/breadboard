#include "unity.h"
#include "bb_timer.h"
#include "bb_timer_periodic_test.h"

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

void test_bb_timer_periodic_create_null_cb_returns_err(void)
{
    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_periodic_create(NULL, NULL, "x", &t);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_bb_timer_periodic_create_null_out_returns_err(void)
{
    bb_err_t err = bb_timer_periodic_create(fire_cb, NULL, "x", NULL);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_bb_timer_periodic_create_start_fire_increments(void)
{
    s_fire_count = 0;

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_periodic_create(fire_cb, NULL, "test", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_fire_count);

    bb_timer_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(2, s_fire_count);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_periodic_stop_prevents_fire(void)
{
    s_fire_count = 0;

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_periodic_create(fire_cb, NULL, "stop_test", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_fire_count);

    err = bb_timer_periodic_stop(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_fire_count);  // count unchanged after stop

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_periodic_restart_after_stop_fires_again(void)
{
    s_fire_count = 0;

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_periodic_create(fire_cb, NULL, "restart", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(1, s_fire_count);

    err = bb_timer_periodic_stop(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Restart with a new period
    err = bb_timer_periodic_start(t, 500000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(2, s_fire_count);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_periodic_arg_passed_to_cb(void)
{
    int count = 0;

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_periodic_create(counter_cb, &count, "arg_test", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_periodic_fire_for_test(t);
    bb_timer_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(2, count);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_periodic_delete_after_stop(void)
{
    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_periodic_create(fire_cb, NULL, "del", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_start(t, 1000000);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_stop(t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_periodic_fire_without_start_is_noop(void)
{
    s_fire_count = 0;

    bb_periodic_timer_t t;
    bb_err_t err = bb_timer_periodic_create(fire_cb, NULL, "nostart", &t);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_timer_periodic_fire_for_test(t);
    TEST_ASSERT_EQUAL(0, s_fire_count);

    err = bb_timer_periodic_delete(t);
    TEST_ASSERT_EQUAL(BB_OK, err);
}
