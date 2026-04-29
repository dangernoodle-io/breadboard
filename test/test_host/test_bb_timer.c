#include "unity.h"
#include "bb_timer.h"
#include <unistd.h>

static volatile int s_count = 0;

static void timer_cb(void *arg)
{
    (void)arg;
    s_count++;
}

void test_bb_timer_create_null_out_returns_err(void)
{
    bb_err_t err = bb_timer_create("test", BB_TIMER_ONE_SHOT, 1000, timer_cb, NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_bb_timer_create_null_cb_returns_err(void)
{
    bb_timer_handle_t h;
    bb_err_t err = bb_timer_create("test", BB_TIMER_ONE_SHOT, 1000, NULL, NULL, &h);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
}

void test_bb_timer_one_shot_fires_once(void)
{
    s_count = 0;
    bb_timer_handle_t h;
    bb_err_t err = bb_timer_create("one_shot", BB_TIMER_ONE_SHOT, 50000, timer_cb, NULL, &h);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_start(h);
    TEST_ASSERT_EQUAL(BB_OK, err);

    usleep(200000);  // Sleep 200ms

    TEST_ASSERT_EQUAL(1, s_count);

    err = bb_timer_delete(h);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_periodic_fires_repeatedly_then_stops(void)
{
    s_count = 0;
    bb_timer_handle_t h;
    bb_err_t err = bb_timer_create("periodic", BB_TIMER_PERIODIC, 30000, timer_cb, NULL, &h);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_start(h);
    TEST_ASSERT_EQUAL(BB_OK, err);

    usleep(100000);  // Sleep 100ms
    int count_before_stop = s_count;
    TEST_ASSERT_GREATER_THAN(0, count_before_stop);

    err = bb_timer_stop(h);
    TEST_ASSERT_EQUAL(BB_OK, err);

    int count_after_stop = s_count;
    usleep(100000);  // Sleep another 100ms
    int count_final = s_count;

    TEST_ASSERT_EQUAL(count_after_stop, count_final);  // No more callbacks

    err = bb_timer_delete(h);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_delete_after_stop(void)
{
    bb_timer_handle_t h;
    bb_err_t err = bb_timer_create("delete_after_stop", BB_TIMER_ONE_SHOT, 1000, timer_cb, NULL, &h);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_start(h);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_stop(h);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_delete(h);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_timer_delete_without_start(void)
{
    bb_timer_handle_t h;
    bb_err_t err = bb_timer_create("delete_no_start", BB_TIMER_ONE_SHOT, 1000, timer_cb, NULL, &h);
    TEST_ASSERT_EQUAL(BB_OK, err);

    err = bb_timer_delete(h);
    TEST_ASSERT_EQUAL(BB_OK, err);
}
