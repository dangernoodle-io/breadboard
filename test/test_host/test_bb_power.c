// Tests for bb_power HAL: vtable dispatch, poll/snapshot, pout_mw, primary/name.
#include "unity.h"
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_power_test.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Fake backend state + vtable
// ---------------------------------------------------------------------------

typedef struct {
    int vout_mv;
    int iout_ma;
    int vin_mv;
    int temp_c;
    int set_vout_last;
    int set_vout_count;
    bb_err_t set_vout_ret;
} fake_state_t;

static fake_state_t g_fake;

static int fake_read_vout(void *s) { return ((fake_state_t *)s)->vout_mv; }
static int fake_read_iout(void *s) { return ((fake_state_t *)s)->iout_ma; }
static int fake_read_vin (void *s) { return ((fake_state_t *)s)->vin_mv; }
static int fake_read_temp(void *s) { return ((fake_state_t *)s)->temp_c; }
static bb_err_t fake_set_vout(void *s, uint16_t mv)
{
    fake_state_t *fs = s;
    fs->set_vout_last = mv;
    fs->set_vout_count++;
    return fs->set_vout_ret;
}

static const bb_power_driver_t drv_full = {
    .read_vout_mv = fake_read_vout,
    .read_iout_ma = fake_read_iout,
    .read_vin_mv  = fake_read_vin,
    .read_temp_c  = fake_read_temp,
    .set_vout_mv  = fake_set_vout,
    .name         = "fake_pwr",
};

static const bb_power_driver_t drv_no_name = {
    .read_vout_mv = fake_read_vout,
    .read_iout_ma = fake_read_iout,
    .read_vin_mv  = fake_read_vin,
    .read_temp_c  = fake_read_temp,
    .set_vout_mv  = fake_set_vout,
    .name         = NULL,
};

// Driver that returns -1 for all reads (error path)
static int err_read(void *s) { (void)s; return -1; }
static bb_err_t err_set_vout(void *s, uint16_t mv) { (void)s; (void)mv; return BB_ERR_INVALID_STATE; }

static const bb_power_driver_t drv_errors = {
    .read_vout_mv = err_read,
    .read_iout_ma = err_read,
    .read_vin_mv  = err_read,
    .read_temp_c  = err_read,
    .set_vout_mv  = err_set_vout,
    .name         = "error_drv",
};

void bb_power_test_reset_local(void)
{
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.set_vout_ret = BB_OK;
    bb_power_test_reset();
}

// ---------------------------------------------------------------------------
// handle_create
// ---------------------------------------------------------------------------

void test_bb_power_handle_create_null_drv(void)
{
    bb_power_handle_t h;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_power_handle_create(NULL, &g_fake, &h));
}

void test_bb_power_handle_create_null_out(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_power_handle_create(&drv_full, &g_fake, NULL));
}

void test_bb_power_handle_create_succeeds(void)
{
    bb_power_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_power_handle_create(&drv_full, &g_fake, &h));
    TEST_ASSERT_NOT_NULL(h);
    free(h);
}

// ---------------------------------------------------------------------------
// poll — reads vtable, computes pout_mw
// ---------------------------------------------------------------------------

void test_bb_power_poll_computes_pout_mw(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);

    g_fake.vout_mv = 1200;
    g_fake.iout_ma = 5000;
    TEST_ASSERT_EQUAL(BB_OK, bb_power_poll(h));

    bb_power_snapshot_t s;
    bb_power_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(1200, s.vout_mv);
    TEST_ASSERT_EQUAL_INT(5000, s.iout_ma);
    // 1200 * 5000 / 1000 = 6000
    TEST_ASSERT_EQUAL_INT(6000, s.pout_mw);

    free(h);
}

void test_bb_power_poll_pout_mw_negative_when_vout_error(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);

    g_fake.vout_mv = -1;
    g_fake.iout_ma = 3000;
    bb_power_poll(h);

    bb_power_snapshot_t s;
    bb_power_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(-1, s.pout_mw);
    free(h);
}

void test_bb_power_poll_pout_mw_negative_when_iout_error(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);

    g_fake.vout_mv = 1000;
    g_fake.iout_ma = -1;
    bb_power_poll(h);

    bb_power_snapshot_t s;
    bb_power_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(-1, s.pout_mw);
    free(h);
}

void test_bb_power_poll_all_error_fields(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_errors, &g_fake, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_power_poll(h));

    bb_power_snapshot_t s;
    bb_power_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(-1, s.vout_mv);
    TEST_ASSERT_EQUAL_INT(-1, s.iout_ma);
    TEST_ASSERT_EQUAL_INT(-1, s.pout_mw);
    TEST_ASSERT_EQUAL_INT(-1, s.vin_mv);
    TEST_ASSERT_EQUAL_INT(-1, s.temp_c);
    free(h);
}

void test_bb_power_poll_null_handle(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_power_poll(NULL));
}

void test_bb_power_poll_updates_all_fields(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);

    g_fake.vout_mv = 850;
    g_fake.iout_ma = 10000;
    g_fake.vin_mv  = 12000;
    g_fake.temp_c  = 45;
    bb_power_poll(h);

    bb_power_snapshot_t s;
    bb_power_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(850,   s.vout_mv);
    TEST_ASSERT_EQUAL_INT(10000, s.iout_ma);
    TEST_ASSERT_EQUAL_INT(8500,  s.pout_mw); // 850*10000/1000
    TEST_ASSERT_EQUAL_INT(12000, s.vin_mv);
    TEST_ASSERT_EQUAL_INT(45,    s.temp_c);
    free(h);
}

// ---------------------------------------------------------------------------
// snapshot — NULL handle returns all -1
// ---------------------------------------------------------------------------

void test_bb_power_snapshot_null_handle_all_minus_one(void)
{
    bb_power_snapshot_t s;
    bb_power_snapshot(NULL, &s);
    TEST_ASSERT_EQUAL_INT(-1, s.vout_mv);
    TEST_ASSERT_EQUAL_INT(-1, s.iout_ma);
    TEST_ASSERT_EQUAL_INT(-1, s.pout_mw);
    TEST_ASSERT_EQUAL_INT(-1, s.vin_mv);
    TEST_ASSERT_EQUAL_INT(-1, s.temp_c);
}

void test_bb_power_snapshot_returns_cache(void)
{
    // snapshot returns the last polled values, not live reads
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);

    g_fake.vout_mv = 1000;
    bb_power_poll(h);

    // Change g_fake after poll — snapshot should still see 1000
    g_fake.vout_mv = 9999;

    bb_power_snapshot_t s;
    bb_power_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(1000, s.vout_mv);
    free(h);
}

// ---------------------------------------------------------------------------
// snapshot — NULL out is safe
// ---------------------------------------------------------------------------

void test_bb_power_snapshot_null_out_is_safe(void)
{
    bb_power_snapshot(NULL, NULL); // must not crash
}

// ---------------------------------------------------------------------------
// set_vout_mv
// ---------------------------------------------------------------------------

void test_bb_power_set_vout_mv_delegates(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_power_set_vout_mv(h, 1100));
    TEST_ASSERT_EQUAL_INT(1100, g_fake.set_vout_last);
    TEST_ASSERT_EQUAL_INT(1, g_fake.set_vout_count);
    free(h);
}

void test_bb_power_set_vout_mv_null_handle(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_power_set_vout_mv(NULL, 1000));
}

void test_bb_power_set_vout_mv_no_fn_unsupported(void)
{
    static const bb_power_driver_t drv_no_set = {
        .read_vout_mv = fake_read_vout,
        .read_iout_ma = fake_read_iout,
        .read_vin_mv  = fake_read_vin,
        .read_temp_c  = fake_read_temp,
        .set_vout_mv  = NULL,
        .name         = "no_set",
    };
    bb_power_handle_t h;
    bb_power_handle_create(&drv_no_set, &g_fake, &h);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_power_set_vout_mv(h, 1000));
    free(h);
}

// ---------------------------------------------------------------------------
// name
// ---------------------------------------------------------------------------

void test_bb_power_name_returns_driver_name(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);
    TEST_ASSERT_EQUAL_STRING("fake_pwr", bb_power_name(h));
    free(h);
}

void test_bb_power_name_null_handle_returns_null(void)
{
    TEST_ASSERT_NULL(bb_power_name(NULL));
}

void test_bb_power_name_null_driver_name_returns_null(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_no_name, &g_fake, &h);
    TEST_ASSERT_NULL(bb_power_name(h));
    free(h);
}

// ---------------------------------------------------------------------------
// primary
// ---------------------------------------------------------------------------

void test_bb_power_primary_null_before_set(void)
{
    TEST_ASSERT_NULL(bb_power_primary());
}

void test_bb_power_set_primary_stores_handle(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);
    bb_power_set_primary(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_power_primary());
    bb_power_set_primary(NULL);
    free(h);
}

void test_bb_power_set_primary_null_clears(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);
    bb_power_set_primary(h);
    bb_power_set_primary(NULL);
    TEST_ASSERT_NULL(bb_power_primary());
    free(h);
}

// ---------------------------------------------------------------------------
// Initial snapshot fields are -1 (before first poll)
// ---------------------------------------------------------------------------

void test_bb_power_initial_snapshot_all_minus_one(void)
{
    bb_power_handle_t h;
    bb_power_handle_create(&drv_full, &g_fake, &h);

    bb_power_snapshot_t s;
    bb_power_snapshot(h, &s);
    TEST_ASSERT_EQUAL_INT(-1, s.vout_mv);
    TEST_ASSERT_EQUAL_INT(-1, s.iout_ma);
    TEST_ASSERT_EQUAL_INT(-1, s.pout_mw);
    TEST_ASSERT_EQUAL_INT(-1, s.vin_mv);
    TEST_ASSERT_EQUAL_INT(-1, s.temp_c);
    free(h);
}
