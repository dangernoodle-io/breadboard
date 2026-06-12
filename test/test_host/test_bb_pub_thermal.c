// Tests for bb_pub_thermal: sample fn publishes expected fields; skips when all absent.
#include "unity.h"
#include "bb_pub_thermal.h"
#include "bb_pub.h"
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_power_test.h"
#include "bb_temp.h"
#include "bb_temp_test.h"
#include "bb_nv.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Fake capturing sink
// ---------------------------------------------------------------------------

#define CAPTURE_CAP 16

typedef struct {
    char topic[192];
    char payload[512];
} capture_entry_t;

static capture_entry_t s_captured[CAPTURE_CAP];
static int             s_capture_count;

static bb_err_t capture_publish(void *ctx, const char *topic,
                                 const char *payload, int len)
{
    (void)ctx;
    (void)len;
    if (s_capture_count >= CAPTURE_CAP) return BB_ERR_NO_SPACE;
    capture_entry_t *e = &s_captured[s_capture_count++];
    strncpy(e->topic,   topic,   sizeof(e->topic)   - 1);
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    return BB_OK;
}

static void capture_reset(void)
{
    memset(s_captured, 0, sizeof(s_captured));
    s_capture_count = 0;
}

// ---------------------------------------------------------------------------
// Fake power backend
// ---------------------------------------------------------------------------

typedef struct {
    int vout_mv, iout_ma, vin_mv, temp_c;
} fake_pwr_t;

static fake_pwr_t g_pwr;

static int fp_vout(void *s) { return ((fake_pwr_t *)s)->vout_mv; }
static int fp_iout(void *s) { return ((fake_pwr_t *)s)->iout_ma; }
static int fp_vin (void *s) { return ((fake_pwr_t *)s)->vin_mv; }
static int fp_temp(void *s) { return ((fake_pwr_t *)s)->temp_c; }
static bb_err_t fp_set(void *s, uint16_t mv) { (void)s; (void)mv; return BB_OK; }

static const bb_power_driver_t drv_pwr = {
    .read_vout_mv = fp_vout,
    .read_iout_ma = fp_iout,
    .read_vin_mv  = fp_vin,
    .read_temp_c  = fp_temp,
    .set_vout_mv  = fp_set,
    .name         = "test_pwr",
};

// ---------------------------------------------------------------------------
// Fake fan backend
// ---------------------------------------------------------------------------

typedef struct {
    int   rpm;
    int   duty_pct;
    float die_c;
    float board_c;
    bool  die_fail;
    bool  board_fail;
} fake_fan_t;

static fake_fan_t g_fan;

static bb_err_t ff_set_duty(void *s, int pct) { (void)s; (void)pct; return BB_OK; }
static int ff_get_duty(void *s) { return ((fake_fan_t *)s)->duty_pct; }
static int ff_rpm(void *s)      { return ((fake_fan_t *)s)->rpm; }

static bb_err_t ff_die(void *s, float *out)
{
    fake_fan_t *f = s;
    if (f->die_fail) return BB_ERR_INVALID_STATE;
    *out = f->die_c;
    return BB_OK;
}
static bb_err_t ff_board(void *s, float *out)
{
    fake_fan_t *f = s;
    if (f->board_fail) return BB_ERR_INVALID_STATE;
    *out = f->board_c;
    return BB_OK;
}

static const bb_fan_driver_t drv_fan = {
    .set_duty_pct      = ff_set_duty,
    .get_duty_pct      = ff_get_duty,
    .read_rpm          = ff_rpm,
    .read_die_temp_c   = ff_die,
    .read_board_temp_c = ff_board,
    .name              = "fake_fan",
};

// ---------------------------------------------------------------------------
// Helper: full reset
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_fan_test_reset();
    bb_power_test_reset();
    bb_temp_test_set_soc(false, 0.0f);
    bb_nv_config_set_hostname("testhost");
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_thermal_publishes_soc_field(void)
{
    reset_all();
    bb_temp_test_set_soc(true, 55.5f);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_thermal_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"soc_c\""));
}

void test_bb_pub_thermal_publishes_vr_field(void)
{
    reset_all();
    g_pwr.vout_mv = 1200;
    g_pwr.iout_ma = 5000;
    g_pwr.vin_mv  = 12000;
    g_pwr.temp_c  = 48;
    bb_power_handle_t ph = NULL;
    bb_power_handle_create(&drv_pwr, &g_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_thermal_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"vr_c\""));
}

void test_bb_pub_thermal_publishes_asic_and_board_fields(void)
{
    reset_all();
    g_fan.rpm      = 2000;
    g_fan.duty_pct = 60;
    g_fan.die_c    = 52.0f;
    g_fan.board_c  = 33.0f;
    g_fan.die_fail = false;
    g_fan.board_fail = false;
    bb_fan_handle_t fh = NULL;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_thermal_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"asic_c\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"board_c\""));
}

void test_bb_pub_thermal_topic_is_correct(void)
{
    reset_all();
    bb_temp_test_set_soc(true, 40.0f);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_thermal_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/thermal", s_captured[0].topic);
}

void test_bb_pub_thermal_skips_when_all_absent(void)
{
    reset_all();
    // No soc, no power primary, no fan primary — all absent.

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_thermal_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_thermal_vr_null_when_temp_minus_one(void)
{
    reset_all();
    g_pwr.vout_mv = 1200;
    g_pwr.iout_ma = 5000;
    g_pwr.vin_mv  = 12000;
    g_pwr.temp_c  = -1;  // unavailable
    bb_power_handle_t ph = NULL;
    bb_power_handle_create(&drv_pwr, &g_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_thermal_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"vr_c\":null"));
}

void test_bb_pub_thermal_asic_null_when_die_nan(void)
{
    reset_all();
    g_fan.rpm      = 1800;
    g_fan.duty_pct = 50;
    g_fan.die_c    = NAN;
    g_fan.board_c  = 30.0f;
    g_fan.die_fail = true;  // signals NAN result
    g_fan.board_fail = false;
    bb_fan_handle_t fh = NULL;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_thermal_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"asic_c\":null"));
}

void test_bb_pub_thermal_payload_has_ts_field(void)
{
    reset_all();
    bb_temp_test_set_soc(true, 35.0f);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_thermal_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts\""));
}
