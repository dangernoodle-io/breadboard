// Tests for bb_thermal_collect(): verifies present/absent per source, hw_present distinctions,
// value pass-through, and all-present branch. (B1-352)
#include "unity.h"
#include "bb_thermal.h"
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_power_test.h"
#include "bb_temp.h"
#include "bb_temp_test.h"
#include <math.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Fake power backend
// ---------------------------------------------------------------------------

typedef struct { int vout_mv, iout_ma, vin_mv, temp_c; } tc_pwr_t;
static tc_pwr_t g_tc_pwr;

static int tc_pwr_vout(void *s) { return ((tc_pwr_t *)s)->vout_mv; }
static int tc_pwr_iout(void *s) { return ((tc_pwr_t *)s)->iout_ma; }
static int tc_pwr_vin (void *s) { return ((tc_pwr_t *)s)->vin_mv;  }
static int tc_pwr_temp(void *s) { return ((tc_pwr_t *)s)->temp_c;  }
static bb_err_t tc_pwr_set(void *s, uint16_t mv) { (void)s; (void)mv; return BB_OK; }

static const bb_power_driver_t tc_pwr_drv = {
    .read_vout_mv = tc_pwr_vout,
    .read_iout_ma = tc_pwr_iout,
    .read_vin_mv  = tc_pwr_vin,
    .read_temp_c  = tc_pwr_temp,
    .set_vout_mv  = tc_pwr_set,
    .name         = "tc_pwr",
};

// ---------------------------------------------------------------------------
// Fake fan backend
// ---------------------------------------------------------------------------

typedef struct {
    int   rpm, duty_pct;
    float die_c, board_c;
    bool  die_fail, board_fail;
} tc_fan_t;
static tc_fan_t g_tc_fan;

static bb_err_t tc_fan_set_duty(void *s, int pct) { (void)s; (void)pct; return BB_OK; }
static int tc_fan_get_duty(void *s) { return ((tc_fan_t *)s)->duty_pct; }
static int tc_fan_rpm(void *s)      { return ((tc_fan_t *)s)->rpm; }

static bb_err_t tc_fan_die(void *s, float *out)
{
    tc_fan_t *f = s;
    if (f->die_fail) return BB_ERR_INVALID_STATE;
    *out = f->die_c;
    return BB_OK;
}
static bb_err_t tc_fan_board(void *s, float *out)
{
    tc_fan_t *f = s;
    if (f->board_fail) return BB_ERR_INVALID_STATE;
    *out = f->board_c;
    return BB_OK;
}

static const bb_fan_driver_t tc_fan_drv = {
    .set_duty_pct      = tc_fan_set_duty,
    .get_duty_pct      = tc_fan_get_duty,
    .read_rpm          = tc_fan_rpm,
    .read_die_temp_c   = tc_fan_die,
    .read_board_temp_c = tc_fan_board,
    .name              = "tc_fan",
};

// ---------------------------------------------------------------------------
// Helper: full reset (global setUp calls bb_thermal_reset_for_test already)
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    bb_thermal_reset_for_test();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_thermal_collect_all_absent(void)
{
    reset_all();
    // No soc, no power primary, no fan primary — all present=false, all hw_present=false.
    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_FALSE(v.soc_present);
    TEST_ASSERT_FALSE(v.vr_hw_present);
    TEST_ASSERT_FALSE(v.vr_present);
    TEST_ASSERT_FALSE(v.fan_hw_present);
    TEST_ASSERT_FALSE(v.asic_present);
    TEST_ASSERT_FALSE(v.board_present);
}

void test_bb_thermal_collect_soc_present(void)
{
    reset_all();
    bb_temp_test_set_soc(true, 42.5f);

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_TRUE(v.soc_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.5f, v.soc_c);
}

void test_bb_thermal_collect_soc_absent(void)
{
    reset_all();
    bb_temp_test_set_soc(false, 0.0f);

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_FALSE(v.soc_present);
}

void test_bb_thermal_collect_vr_present_with_reading(void)
{
    reset_all();
    g_tc_pwr.vout_mv = 1200;
    g_tc_pwr.iout_ma = 2000;
    g_tc_pwr.vin_mv  = 5000;
    g_tc_pwr.temp_c  = 75;

    bb_power_handle_t ph = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_power_handle_create(&tc_pwr_drv, &g_tc_pwr, &ph));
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_TRUE(v.vr_hw_present);
    TEST_ASSERT_TRUE(v.vr_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 75.0f, v.vr_c);
}

void test_bb_thermal_collect_vr_hw_present_no_reading(void)
{
    reset_all();
    g_tc_pwr.temp_c = -1;  // -1 means no reading

    bb_power_handle_t ph = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_power_handle_create(&tc_pwr_drv, &g_tc_pwr, &ph));
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_TRUE(v.vr_hw_present);
    TEST_ASSERT_FALSE(v.vr_present);
}

void test_bb_thermal_collect_asic_present(void)
{
    reset_all();
    g_tc_fan.rpm      = 2000;
    g_tc_fan.duty_pct = 50;
    g_tc_fan.die_c    = 68.0f;
    g_tc_fan.board_c  = 35.0f;
    g_tc_fan.die_fail  = false;
    g_tc_fan.board_fail = false;

    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&tc_fan_drv, &g_tc_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_TRUE(v.fan_hw_present);
    TEST_ASSERT_TRUE(v.asic_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 68.0f, v.asic_c);
}

void test_bb_thermal_collect_board_present(void)
{
    reset_all();
    g_tc_fan.rpm      = 2000;
    g_tc_fan.duty_pct = 50;
    g_tc_fan.die_c    = 68.0f;
    g_tc_fan.board_c  = 35.5f;
    g_tc_fan.die_fail  = false;
    g_tc_fan.board_fail = false;

    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&tc_fan_drv, &g_tc_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_TRUE(v.fan_hw_present);
    TEST_ASSERT_TRUE(v.board_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 35.5f, v.board_c);
}

void test_bb_thermal_collect_asic_absent_when_die_nan(void)
{
    reset_all();
    g_tc_fan.rpm      = 2000;
    g_tc_fan.duty_pct = 50;
    g_tc_fan.board_c  = 35.0f;
    g_tc_fan.die_fail  = true;   // die read fails → NaN in snapshot
    g_tc_fan.board_fail = false;

    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&tc_fan_drv, &g_tc_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_TRUE(v.fan_hw_present);
    TEST_ASSERT_FALSE(v.asic_present);
}

void test_bb_thermal_collect_board_absent_when_board_nan(void)
{
    reset_all();
    g_tc_fan.rpm      = 2000;
    g_tc_fan.duty_pct = 50;
    g_tc_fan.die_c    = 68.0f;
    g_tc_fan.die_fail  = false;
    g_tc_fan.board_fail = true;  // board read fails → NaN in snapshot

    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&tc_fan_drv, &g_tc_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_TRUE(v.fan_hw_present);
    TEST_ASSERT_FALSE(v.board_present);
}

void test_bb_thermal_collect_all_present(void)
{
    reset_all();
    bb_temp_test_set_soc(true, 55.0f);

    g_tc_pwr.temp_c = 80;
    bb_power_handle_t ph = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_power_handle_create(&tc_pwr_drv, &g_tc_pwr, &ph));
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    g_tc_fan.die_c    = 70.0f;
    g_tc_fan.board_c  = 40.0f;
    g_tc_fan.die_fail  = false;
    g_tc_fan.board_fail = false;
    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&tc_fan_drv, &g_tc_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_thermal_values_t v;
    bb_thermal_collect(&v);

    TEST_ASSERT_TRUE(v.soc_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 55.0f, v.soc_c);

    TEST_ASSERT_TRUE(v.vr_hw_present);
    TEST_ASSERT_TRUE(v.vr_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, v.vr_c);

    TEST_ASSERT_TRUE(v.fan_hw_present);
    TEST_ASSERT_TRUE(v.asic_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 70.0f, v.asic_c);
    TEST_ASSERT_TRUE(v.board_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, v.board_c);
}
