// Tests for bb_fan HAL: vtable dispatch, poll/snapshot, primary/name, set/get_duty.
#include "unity.h"
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Fake backend state + vtable
// ---------------------------------------------------------------------------

typedef struct {
    int rpm;
    int duty_pct;
    float die_c;
    float board_c;
    bool die_fail;
    bool board_fail;
    int set_duty_last;
    int set_duty_count;
    bb_err_t set_duty_ret;
} fake_fan_state_t;

static fake_fan_state_t g_fake;

static bb_err_t fake_set_duty(void *s, int pct)
{
    fake_fan_state_t *fs = s;
    fs->set_duty_last = pct;
    fs->set_duty_count++;
    fs->duty_pct = pct;
    return fs->set_duty_ret;
}

static int fake_get_duty(void *s) { return ((fake_fan_state_t *)s)->duty_pct; }
static int fake_read_rpm(void *s) { return ((fake_fan_state_t *)s)->rpm; }

static bb_err_t fake_read_die(void *s, float *out)
{
    fake_fan_state_t *fs = s;
    if (fs->die_fail) return BB_ERR_INVALID_STATE;
    *out = fs->die_c;
    return BB_OK;
}

static bb_err_t fake_read_board(void *s, float *out)
{
    fake_fan_state_t *fs = s;
    if (fs->board_fail) return BB_ERR_INVALID_STATE;
    *out = fs->board_c;
    return BB_OK;
}

static const bb_fan_driver_t drv_full = {
    .set_duty_pct     = fake_set_duty,
    .get_duty_pct     = fake_get_duty,
    .read_rpm         = fake_read_rpm,
    .read_die_temp_c  = fake_read_die,
    .read_board_temp_c = fake_read_board,
    .name             = "fake_fan",
};

static const bb_fan_driver_t drv_no_name = {
    .set_duty_pct     = fake_set_duty,
    .get_duty_pct     = fake_get_duty,
    .read_rpm         = fake_read_rpm,
    .read_die_temp_c  = fake_read_die,
    .read_board_temp_c = fake_read_board,
    .name             = NULL,
};

// Driver with no optional functions
static const bb_fan_driver_t drv_minimal = {
    .set_duty_pct     = NULL,
    .get_duty_pct     = NULL,
    .read_rpm         = NULL,
    .read_die_temp_c  = NULL,
    .read_board_temp_c = NULL,
    .name             = "minimal",
};

void bb_fan_test_reset_local(void)
{
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.set_duty_ret = BB_OK;
    g_fake.duty_pct     = -1;
    bb_fan_test_reset();
}

// ---------------------------------------------------------------------------
// handle_create
// ---------------------------------------------------------------------------

void test_bb_fan_handle_create_null_drv(void)
{
    bb_fan_handle_t h;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fan_handle_create(NULL, &g_fake, &h));
}

void test_bb_fan_handle_create_null_out(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_fan_handle_create(&drv_full, &g_fake, NULL));
}

void test_bb_fan_handle_create_succeeds(void)
{
    bb_fan_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&drv_full, &g_fake, &h));
    TEST_ASSERT_NOT_NULL(h);
    free(h);
}

// ---------------------------------------------------------------------------
// poll — vtable dispatch + cache
// ---------------------------------------------------------------------------

void test_bb_fan_poll_caches_rpm_and_duty(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);

    g_fake.rpm      = 3200;
    g_fake.die_c    = 72.5f;
    g_fake.board_c  = 35.0f;

#ifdef CONFIG_BB_FAN_AUTOFAN
    // When autofan is compiled in, BB owns duty: poll applies manual_pct (default 100).
    g_fake.duty_pct = 3200; // irrelevant — will be overwritten by manual_pct
#else
    g_fake.duty_pct = 75;
#endif

    TEST_ASSERT_EQUAL(BB_OK, bb_fan_poll(h));

    bb_fan_snapshot_t s;
    bb_fan_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(3200, s.rpm);
#ifdef CONFIG_BB_FAN_AUTOFAN
    TEST_ASSERT_EQUAL_INT(100, s.duty_pct); // default manual_pct=100
#else
    TEST_ASSERT_EQUAL_INT(75, s.duty_pct);
#endif
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 72.5f, s.die_c);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 35.0f, s.board_c);

    free(h);
}

void test_bb_fan_poll_die_fail_gives_nan(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);

    g_fake.die_fail   = true;
    g_fake.board_fail = false;
    g_fake.board_c    = 30.0f;
    bb_fan_poll(h);

    bb_fan_snapshot_t s;
    bb_fan_snapshot(h, &s);
    TEST_ASSERT_TRUE(isnan(s.die_c));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, s.board_c);

    free(h);
}

void test_bb_fan_poll_board_fail_gives_nan(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);

    g_fake.board_fail = true;
    g_fake.die_c      = 65.0f;
    bb_fan_poll(h);

    bb_fan_snapshot_t s;
    bb_fan_snapshot(h, &s);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 65.0f, s.die_c);
    TEST_ASSERT_TRUE(isnan(s.board_c));

    free(h);
}

void test_bb_fan_poll_null_fns_give_sentinel(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_minimal, &g_fake, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_poll(h));

    bb_fan_snapshot_t s;
    bb_fan_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(-1, s.rpm);
    TEST_ASSERT_EQUAL_INT(-1, s.duty_pct);
    TEST_ASSERT_TRUE(isnan(s.die_c));
    TEST_ASSERT_TRUE(isnan(s.board_c));

    free(h);
}

void test_bb_fan_poll_null_handle(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_fan_poll(NULL));
}

// ---------------------------------------------------------------------------
// snapshot — NULL handle returns sentinels
// ---------------------------------------------------------------------------

void test_bb_fan_snapshot_null_handle_sentinels(void)
{
    bb_fan_snapshot_t s;
    bb_fan_snapshot(NULL, &s);
    TEST_ASSERT_EQUAL_INT(-1, s.rpm);
    TEST_ASSERT_EQUAL_INT(-1, s.duty_pct);
    TEST_ASSERT_TRUE(isnan(s.die_c));
    TEST_ASSERT_TRUE(isnan(s.board_c));
}

void test_bb_fan_snapshot_returns_cache(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);

    g_fake.rpm = 1000;
    bb_fan_poll(h);

    g_fake.rpm = 9999;  // change after poll — snapshot should see 1000
    bb_fan_snapshot_t s;
    bb_fan_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(1000, s.rpm);

    free(h);
}

void test_bb_fan_snapshot_null_out_is_safe(void)
{
    bb_fan_snapshot(NULL, NULL);  // must not crash
}

// ---------------------------------------------------------------------------
// Initial snapshot (before first poll)
// ---------------------------------------------------------------------------

void test_bb_fan_initial_snapshot_sentinels(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);

    bb_fan_snapshot_t s;
    bb_fan_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(-1, s.rpm);
    TEST_ASSERT_EQUAL_INT(-1, s.duty_pct);
    TEST_ASSERT_TRUE(isnan(s.die_c));
    TEST_ASSERT_TRUE(isnan(s.board_c));

    free(h);
}

// ---------------------------------------------------------------------------
// set/get_duty_pct
// ---------------------------------------------------------------------------

void test_bb_fan_set_duty_pct_delegates(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_set_duty_pct(h, 50));
    TEST_ASSERT_EQUAL_INT(50, g_fake.set_duty_last);
    TEST_ASSERT_EQUAL_INT(1,  g_fake.set_duty_count);
    free(h);
}

void test_bb_fan_set_duty_pct_null_handle(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_fan_set_duty_pct(NULL, 50));
}

void test_bb_fan_set_duty_pct_no_fn_unsupported(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_minimal, &g_fake, &h);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_fan_set_duty_pct(h, 50));
    free(h);
}

void test_bb_fan_get_duty_pct_from_cache(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);

    g_fake.duty_pct = 60;
    bb_fan_poll(h);
#ifdef CONFIG_BB_FAN_AUTOFAN
    // BB owns duty when autofan compiled in: returns manual_pct (default 100).
    TEST_ASSERT_EQUAL_INT(100, bb_fan_get_duty_pct(h));
#else
    TEST_ASSERT_EQUAL_INT(60, bb_fan_get_duty_pct(h));
#endif

    free(h);
}

void test_bb_fan_get_duty_pct_null_handle(void)
{
    TEST_ASSERT_EQUAL_INT(-1, bb_fan_get_duty_pct(NULL));
}

// ---------------------------------------------------------------------------
// name
// ---------------------------------------------------------------------------

void test_bb_fan_name_returns_driver_name(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);
    TEST_ASSERT_EQUAL_STRING("fake_fan", bb_fan_name(h));
    free(h);
}

void test_bb_fan_name_null_handle_returns_null(void)
{
    TEST_ASSERT_NULL(bb_fan_name(NULL));
}

void test_bb_fan_name_null_driver_name_returns_null(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_no_name, &g_fake, &h);
    TEST_ASSERT_NULL(bb_fan_name(h));
    free(h);
}

// ---------------------------------------------------------------------------
// primary
// ---------------------------------------------------------------------------

void test_bb_fan_primary_null_before_set(void)
{
    TEST_ASSERT_NULL(bb_fan_primary());
}

void test_bb_fan_set_primary_stores_handle(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);
    bb_fan_set_primary(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_fan_primary());
    bb_fan_set_primary(NULL);
    free(h);
}

void test_bb_fan_set_primary_null_clears(void)
{
    bb_fan_handle_t h;
    bb_fan_handle_create(&drv_full, &g_fake, &h);
    bb_fan_set_primary(h);
    bb_fan_set_primary(NULL);
    TEST_ASSERT_NULL(bb_fan_primary());
    free(h);
}
