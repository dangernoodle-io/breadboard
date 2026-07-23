// Tests for bb_sensor's domain snapshot getters (fan/power/thermal) + the
// fan write path (bb_sensor_fan_apply). All HAL-facing logic that used to
// live in bb_thermal_collect() (B1-352) and bb_sensor_http_wire.c's
// fan/power gather + fan apply (B1-828 PR-2) now lives here -- the wire
// layer (test_bb_sensor_http.c) only proves the thin wire-copy adapters.
#include "unity.h"

#include "bb_sensor.h"
#include "bb_sensor_test.h"

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
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Fake power backend
// ---------------------------------------------------------------------------

typedef struct { int vout_mv, iout_ma, vin_mv, temp_c; } ts_pwr_t;
static ts_pwr_t g_ts_pwr;

static int ts_pwr_vout(void *s) { return ((ts_pwr_t *)s)->vout_mv; }
static int ts_pwr_iout(void *s) { return ((ts_pwr_t *)s)->iout_ma; }
static int ts_pwr_vin (void *s) { return ((ts_pwr_t *)s)->vin_mv;  }
static int ts_pwr_temp(void *s) { return ((ts_pwr_t *)s)->temp_c;  }
static bb_err_t ts_pwr_set(void *s, uint16_t mv) { (void)s; (void)mv; return BB_OK; }

static const bb_power_driver_t ts_pwr_drv = {
    .read_vout_mv = ts_pwr_vout,
    .read_iout_ma = ts_pwr_iout,
    .read_vin_mv  = ts_pwr_vin,
    .read_temp_c  = ts_pwr_temp,
    .set_vout_mv  = ts_pwr_set,
    .name         = "ts_pwr",
};

// ---------------------------------------------------------------------------
// Fake fan backend
// ---------------------------------------------------------------------------

typedef struct {
    int   rpm, duty_pct;
    float die_c, board_c;
    bool  die_fail, board_fail;
    int   set_duty_last;
    bb_err_t set_duty_ret;
} ts_fan_t;
static ts_fan_t g_ts_fan;

static bb_err_t ts_fan_set_duty(void *s, int pct)
{
    ts_fan_t *f = s;
    f->set_duty_last = pct;
    f->duty_pct      = pct;
    return f->set_duty_ret;
}
static int ts_fan_get_duty(void *s) { return ((ts_fan_t *)s)->duty_pct; }
static int ts_fan_rpm(void *s)      { return ((ts_fan_t *)s)->rpm; }

static bb_err_t ts_fan_die(void *s, float *out)
{
    ts_fan_t *f = s;
    if (f->die_fail) return BB_ERR_INVALID_STATE;
    *out = f->die_c;
    return BB_OK;
}
static bb_err_t ts_fan_board(void *s, float *out)
{
    ts_fan_t *f = s;
    if (f->board_fail) return BB_ERR_INVALID_STATE;
    *out = f->board_c;
    return BB_OK;
}

static const bb_fan_driver_t ts_fan_drv = {
    .set_duty_pct      = ts_fan_set_duty,
    .get_duty_pct      = ts_fan_get_duty,
    .read_rpm          = ts_fan_rpm,
    .read_die_temp_c   = ts_fan_die,
    .read_board_temp_c = ts_fan_board,
    .name              = "ts_fan",
};

// ---------------------------------------------------------------------------
// Shared reset
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    bb_sensor_reset_for_test();
}

// ===========================================================================
// bb_sensor_thermal_snapshot -- absorbed from bb_thermal_collect (B1-352)
// ===========================================================================

void test_bb_sensor_thermal_snapshot_all_absent(void)
{
    reset_all();
    // No soc, no power primary, no fan primary — all present=false, all hw_present=false.
    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    TEST_ASSERT_FALSE(v.soc_present);
    TEST_ASSERT_FALSE(v.vr_hw_present);
    TEST_ASSERT_FALSE(v.vr_present);
    TEST_ASSERT_FALSE(v.fan_hw_present);
    TEST_ASSERT_FALSE(v.asic_present);
    TEST_ASSERT_FALSE(v.board_present);
}

void test_bb_sensor_thermal_snapshot_soc_present(void)
{
    reset_all();
    bb_temp_test_set_soc(true, 42.5f);

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    TEST_ASSERT_TRUE(v.soc_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.5f, v.soc_c);
}

void test_bb_sensor_thermal_snapshot_soc_absent(void)
{
    reset_all();
    bb_temp_test_set_soc(false, 0.0f);

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    TEST_ASSERT_FALSE(v.soc_present);
}

void test_bb_sensor_thermal_snapshot_vr_present_with_reading(void)
{
    reset_all();
    g_ts_pwr.vout_mv = 1200;
    g_ts_pwr.iout_ma = 2000;
    g_ts_pwr.vin_mv  = 5000;
    g_ts_pwr.temp_c  = 75;

    bb_power_handle_t ph = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_power_handle_create(&ts_pwr_drv, &g_ts_pwr, &ph));
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    TEST_ASSERT_TRUE(v.vr_hw_present);
    TEST_ASSERT_TRUE(v.vr_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 75.0f, v.vr_c);

    bb_power_set_primary(NULL);
    free(ph);
}

void test_bb_sensor_thermal_snapshot_vr_hw_present_no_reading(void)
{
    reset_all();
    g_ts_pwr.temp_c = -1;  // -1 means no reading

    bb_power_handle_t ph = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_power_handle_create(&ts_pwr_drv, &g_ts_pwr, &ph));
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    TEST_ASSERT_TRUE(v.vr_hw_present);
    TEST_ASSERT_FALSE(v.vr_present);

    bb_power_set_primary(NULL);
    free(ph);
}

void test_bb_sensor_thermal_snapshot_asic_present(void)
{
    reset_all();
    g_ts_fan.rpm      = 2000;
    g_ts_fan.duty_pct = 50;
    g_ts_fan.die_c    = 68.0f;
    g_ts_fan.board_c  = 35.0f;
    g_ts_fan.die_fail  = false;
    g_ts_fan.board_fail = false;

    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&ts_fan_drv, &g_ts_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    TEST_ASSERT_TRUE(v.fan_hw_present);
    TEST_ASSERT_TRUE(v.asic_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 68.0f, v.asic_c);

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_thermal_snapshot_board_present(void)
{
    reset_all();
    g_ts_fan.rpm      = 2000;
    g_ts_fan.duty_pct = 50;
    g_ts_fan.die_c    = 68.0f;
    g_ts_fan.board_c  = 35.5f;
    g_ts_fan.die_fail  = false;
    g_ts_fan.board_fail = false;

    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&ts_fan_drv, &g_ts_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    TEST_ASSERT_TRUE(v.fan_hw_present);
    TEST_ASSERT_TRUE(v.board_present);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 35.5f, v.board_c);

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_thermal_snapshot_asic_absent_when_die_nan(void)
{
    reset_all();
    g_ts_fan.rpm      = 2000;
    g_ts_fan.duty_pct = 50;
    g_ts_fan.board_c  = 35.0f;
    g_ts_fan.die_fail  = true;   // die read fails → NaN in snapshot
    g_ts_fan.board_fail = false;

    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&ts_fan_drv, &g_ts_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    TEST_ASSERT_TRUE(v.fan_hw_present);
    TEST_ASSERT_FALSE(v.asic_present);

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_thermal_snapshot_board_absent_when_board_nan(void)
{
    reset_all();
    g_ts_fan.rpm      = 2000;
    g_ts_fan.duty_pct = 50;
    g_ts_fan.die_c    = 68.0f;
    g_ts_fan.die_fail  = false;
    g_ts_fan.board_fail = true;  // board read fails → NaN in snapshot

    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&ts_fan_drv, &g_ts_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

    TEST_ASSERT_TRUE(v.fan_hw_present);
    TEST_ASSERT_FALSE(v.board_present);

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_thermal_snapshot_all_present(void)
{
    reset_all();
    bb_temp_test_set_soc(true, 55.0f);

    g_ts_pwr.temp_c = 80;
    bb_power_handle_t ph = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_power_handle_create(&ts_pwr_drv, &g_ts_pwr, &ph));
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    g_ts_fan.die_c    = 70.0f;
    g_ts_fan.board_c  = 40.0f;
    g_ts_fan.die_fail  = false;
    g_ts_fan.board_fail = false;
    bb_fan_handle_t fh = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_fan_handle_create(&ts_fan_drv, &g_ts_fan, &fh));
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_sensor_thermal_snapshot_t v;
    bb_sensor_thermal_snapshot(&v);

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

    bb_power_set_primary(NULL);
    free(ph);
    bb_fan_set_primary(NULL);
    free(fh);
}

// ===========================================================================
// bb_sensor_power_snapshot
// ===========================================================================

void test_bb_sensor_power_snapshot_no_primary_all_absent(void)
{
    reset_all();

    bb_sensor_power_snapshot_t snap;
    bb_sensor_power_snapshot(&snap);
    TEST_ASSERT_FALSE(snap.present);
    TEST_ASSERT_TRUE(snap.vout_mv < 0);
    TEST_ASSERT_TRUE(snap.iout_ma < 0);
    TEST_ASSERT_TRUE(snap.vin_mv < 0);
    TEST_ASSERT_TRUE(snap.temp_c < 0);
}

void test_bb_sensor_power_snapshot_with_primary_present(void)
{
    reset_all();

    g_ts_pwr.vout_mv = 1200; g_ts_pwr.iout_ma = 500; g_ts_pwr.vin_mv = 12000; g_ts_pwr.temp_c = 45;
    bb_power_handle_t ph;
    bb_power_handle_create(&ts_pwr_drv, &g_ts_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_sensor_power_snapshot_t snap;
    bb_sensor_power_snapshot(&snap);
    TEST_ASSERT_TRUE(snap.present);
    TEST_ASSERT_EQUAL_INT(1200, snap.vout_mv);
    TEST_ASSERT_EQUAL_INT(500, snap.iout_ma);
    TEST_ASSERT_EQUAL_INT(12000, snap.vin_mv);
    TEST_ASSERT_EQUAL_INT(45, snap.temp_c);
    // pout_mw = (1200*500)/1000 = 600, per bb_power_snapshot's own SSOT calc.
    TEST_ASSERT_EQUAL_INT(600, snap.pout_mw);

    bb_power_set_primary(NULL);
    free(ph);
}

// ===========================================================================
// bb_sensor_fan_snapshot / bb_sensor_fan_apply (autofan build --
// non-autofan's #else branch mirrors the same shape and is exercised by the
// espidf-target CI matrix build, same convention as the pre-consolidation
// test file).
// ===========================================================================

#ifdef CONFIG_BB_FAN_AUTOFAN

static bb_fan_handle_t make_autofan_handle(void)
{
    bb_fan_handle_t fh;
    g_ts_fan.die_fail = g_ts_fan.board_fail = true;
    bb_fan_handle_create(&ts_fan_drv, &g_ts_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);
    return fh;
}

void test_bb_sensor_fan_snapshot_no_primary_present_false(void)
{
    reset_all();

    bb_sensor_fan_snapshot_t snap;
    // No primary fan is an ordinary hardware state -- must NOT be reported
    // as a failure; present=false is how absence is reported. Regression
    // pin: reverting bb_sensor_fan_snapshot()'s no-primary branch back to a
    // failure return turns this red.
    bb_sensor_fan_snapshot(&snap);
    TEST_ASSERT_FALSE(snap.present);
}

void test_bb_sensor_fan_snapshot_reads_live_autofan_cfg(void)
{
    reset_all();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_fan_autofan_cfg_t cfg = { .enabled = true, .die_target_c = 65.0f,
                                  .aux_target_c = 70.0f, .min_pct = 20, .manual_pct = 80 };
    bb_fan_set_autofan(fh, &cfg);

    bb_sensor_fan_snapshot_t snap;
    bb_sensor_fan_snapshot(&snap);
    TEST_ASSERT_TRUE(snap.present);
    TEST_ASSERT_TRUE(snap.autofan);
    TEST_ASSERT_EQUAL_FLOAT(65.0f, snap.die_target_c);
    TEST_ASSERT_EQUAL_FLOAT(70.0f, snap.aux_target_c);
    TEST_ASSERT_EQUAL_INT(20, snap.min_pct);
    TEST_ASSERT_EQUAL_INT(80, snap.manual_pct);

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_fan_apply_no_primary_returns_unsupported(void)
{
    reset_all();

    bb_sensor_fan_snapshot_t cfg = { .autofan = false, .die_target_c = 60.0f, .aux_target_c = 70.0f,
                                      .manual_pct = 50, .min_pct = 10 };
    // BB_ERR_UNSUPPORTED (not BB_ERR_INVALID_STATE) is what lets the shared
    // HTTP status mapper's commit-stage override land this on 503 -- see
    // bb_sensor_fan_apply()'s own doc.
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_sensor_fan_apply(&cfg));
}

void test_bb_sensor_fan_apply_valid_sets_autofan_cfg(void)
{
    reset_all();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_sensor_fan_snapshot_t cfg = { .autofan = true, .die_target_c = 62.0f, .aux_target_c = 71.0f,
                                      .manual_pct = 33, .min_pct = 12 };
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_fan_apply(&cfg));

    bb_fan_autofan_cfg_t hal_cfg;
    bb_fan_get_autofan_cfg(fh, &hal_cfg);
    TEST_ASSERT_TRUE(hal_cfg.enabled);
    TEST_ASSERT_EQUAL_FLOAT(62.0f, hal_cfg.die_target_c);
    TEST_ASSERT_EQUAL_FLOAT(71.0f, hal_cfg.aux_target_c);
    TEST_ASSERT_EQUAL_INT(33, hal_cfg.manual_pct);
    TEST_ASSERT_EQUAL_INT(12, hal_cfg.min_pct);

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_fan_apply_die_target_zero_rejected(void)
{
    reset_all();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_sensor_fan_snapshot_t cfg = { .autofan = false, .die_target_c = 0.0f, .aux_target_c = 70.0f,
                                      .manual_pct = 50, .min_pct = 10 };
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_sensor_fan_apply(&cfg));

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_fan_apply_vr_target_negative_rejected(void)
{
    reset_all();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_sensor_fan_snapshot_t cfg = { .autofan = false, .die_target_c = 60.0f, .aux_target_c = -1.0f,
                                      .manual_pct = 50, .min_pct = 10 };
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_sensor_fan_apply(&cfg));

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_fan_apply_manual_pct_over_100_rejected(void)
{
    reset_all();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_sensor_fan_snapshot_t cfg = { .autofan = false, .die_target_c = 60.0f, .aux_target_c = 70.0f,
                                      .manual_pct = 101, .min_pct = 10 };
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_sensor_fan_apply(&cfg));

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_fan_apply_manual_pct_negative_rejected(void)
{
    reset_all();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_sensor_fan_snapshot_t cfg = { .autofan = false, .die_target_c = 60.0f, .aux_target_c = 70.0f,
                                      .manual_pct = -1, .min_pct = 10 };
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_sensor_fan_apply(&cfg));

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_fan_apply_min_pct_over_100_rejected(void)
{
    reset_all();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_sensor_fan_snapshot_t cfg = { .autofan = false, .die_target_c = 60.0f, .aux_target_c = 70.0f,
                                      .manual_pct = 50, .min_pct = 200 };
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_sensor_fan_apply(&cfg));

    bb_fan_set_primary(NULL);
    free(fh);
}

void test_bb_sensor_fan_apply_min_pct_negative_rejected(void)
{
    reset_all();
    bb_fan_handle_t fh = make_autofan_handle();

    bb_sensor_fan_snapshot_t cfg = { .autofan = false, .die_target_c = 60.0f, .aux_target_c = 70.0f,
                                      .manual_pct = 50, .min_pct = -1 };
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_sensor_fan_apply(&cfg));

    bb_fan_set_primary(NULL);
    free(fh);
}

#else /* !CONFIG_BB_FAN_AUTOFAN */

// Driver bound but missing the optional set_duty_pct vtable slot -- a
// legitimate capability gap (see platform/host/bb_fan/bb_fan.c and
// test/test_host/test_bb_fan.c's drv_minimal coverage), DISTINCT from "no
// primary fan".
static const bb_fan_driver_t ts_fan_drv_no_duty = {
    .set_duty_pct      = NULL,
    .get_duty_pct      = ts_fan_get_duty,
    .read_rpm          = ts_fan_rpm,
    .read_die_temp_c   = ts_fan_die,
    .read_board_temp_c = ts_fan_board,
    .name              = "ts_fan_no_duty",
};

void test_bb_sensor_fan_apply_driver_capability_gap_returns_invalid_state(void)
{
    reset_all();
    bb_fan_handle_t fh;
    bb_fan_handle_create(&ts_fan_drv_no_duty, &g_ts_fan, &fh);
    bb_fan_set_primary(fh);

    bb_sensor_fan_snapshot_t cfg = { .duty_pct = 50 };
    // A primary fan IS wired but its driver can't do duty -- must map to
    // BB_ERR_INVALID_STATE (-> 500), not BB_ERR_UNSUPPORTED (-> 503), so it
    // stays distinguishable from the no-primary-fan case above, which owns
    // the namespace's single unsupported_status override (see
    // bb_sensor_fan_apply()'s own doc). Regression pin: reverting the
    // BB_ERR_UNSUPPORTED->BB_ERR_INVALID_STATE translation in
    // bb_sensor_fan_apply()'s #else branch collapses this back onto
    // BB_ERR_UNSUPPORTED, turning this red.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_sensor_fan_apply(&cfg));

    bb_fan_set_primary(NULL);
    free(fh);
}

#endif /* CONFIG_BB_FAN_AUTOFAN */
