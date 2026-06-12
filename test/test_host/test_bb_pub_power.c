// Tests for bb_pub_power: sample fn publishes expected fields; skips when no primary.
#include "unity.h"
#include "bb_pub_power.h"
#include "bb_pub.h"
#include "bb_power.h"
#include "bb_power_driver.h"
#include "bb_power_test.h"
#include "bb_nv.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
// Helpers
// ---------------------------------------------------------------------------

static void setup_with_power(int vout, int iout, int vin, int temp)
{
    bb_pub_test_reset();
    capture_reset();
    bb_power_test_reset();
    bb_nv_config_set_hostname("testhost");

    g_pwr.vout_mv = vout;
    g_pwr.iout_ma = iout;
    g_pwr.vin_mv  = vin;
    g_pwr.temp_c  = temp;

    bb_power_handle_t ph = NULL;
    bb_power_handle_create(&drv_pwr, &g_pwr, &ph);
    bb_power_poll(ph);
    bb_power_set_primary(ph);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);

    bb_pub_power_register();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_power_publishes_expected_fields(void)
{
    setup_with_power(1200, 5000, 12000, 45);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"vout_mv\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"iout_ma\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"pout_mw\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"vin_mv\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"temp_c\""));
}

void test_bb_pub_power_topic_is_correct(void)
{
    setup_with_power(1000, 4000, 11000, 40);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/power", s_captured[0].topic);
}

void test_bb_pub_power_vout_value_present(void)
{
    setup_with_power(1350, 6000, 12000, 50);
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "1350"));
}

void test_bb_pub_power_vout_null_when_minus_one(void)
{
    setup_with_power(-1, 5000, 12000, 45);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"vout_mv\":null"));
}

void test_bb_pub_power_temp_null_when_minus_one(void)
{
    setup_with_power(1200, 5000, 12000, -1);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"temp_c\":null"));
}

void test_bb_pub_power_pout_mw_computed(void)
{
    // pout_mw = (1200 * 5000) / 1000 = 6000
    setup_with_power(1200, 5000, 12000, 45);
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "6000"));
}

void test_bb_pub_power_skips_when_no_primary(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_power_test_reset();
    bb_nv_config_set_hostname("testhost");

    // No primary set.
    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_power_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_power_payload_has_ts_field(void)
{
    setup_with_power(1100, 4500, 11500, 42);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts\""));
}
