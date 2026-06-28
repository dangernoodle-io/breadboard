// Tests for bb_pub_fan: sample fn publishes expected fields; skips when no primary.
#include "unity.h"
#include "bb_pub_fan.h"
#include "bb_pub.h"
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_fan_test.h"
#include "bb_nv.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Fake capturing sink (same pattern as test_bb_pub.c)
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
    .name              = "test_fan",
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void setup_with_fan(int rpm, int duty, float die, float board)
{
    bb_pub_test_reset();
    capture_reset();
    bb_fan_test_reset();
    bb_nv_config_set_hostname("testhost");

    g_fan.rpm   = rpm;
    g_fan.duty_pct = duty;
    g_fan.die_c  = die;
    g_fan.board_c = board;
    g_fan.die_fail   = isnan(die);
    g_fan.board_fail = isnan(board);

    bb_fan_handle_t fh = NULL;
    bb_fan_handle_create(&drv_fan, &g_fan, &fh);
    bb_fan_poll(fh);
    bb_fan_set_primary(fh);

    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);

    bb_pub_fan_register();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_fan_publishes_expected_fields(void)
{
    setup_with_fan(2400, 75, 45.5f, 32.0f);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"rpm\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"duty_pct\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"die_c\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"board_c\""));
}

void test_bb_pub_fan_topic_is_correct(void)
{
    setup_with_fan(1200, 50, 40.0f, 30.0f);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/fan", s_captured[0].topic);
}

void test_bb_pub_fan_rpm_value_present(void)
{
    setup_with_fan(3000, 80, 50.0f, 35.0f);
    bb_pub_tick_once();
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "3000"));
}

void test_bb_pub_fan_rpm_null_when_minus_one(void)
{
    setup_with_fan(-1, 50, 40.0f, 30.0f);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // rpm should be null
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"rpm\":null"));
}

void test_bb_pub_fan_die_c_null_when_nan(void)
{
    setup_with_fan(2000, 60, NAN, 28.0f);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"die_c\":null"));
}

void test_bb_pub_fan_board_c_null_when_nan(void)
{
    setup_with_fan(2000, 60, 42.0f, NAN);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"board_c\":null"));
}

void test_bb_pub_fan_skips_when_no_primary(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_fan_test_reset();
    bb_nv_config_set_hostname("testhost");

    // No primary set.
    bb_pub_sink_t sink = { .publish = capture_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_fan_register();

    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_fan_payload_has_uptime_ms_field(void)
{
    setup_with_fan(1500, 55, 38.0f, 26.0f);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts_ms\""));
}
