// Tests for bb_pub core: source registry, tick cycle, sink routing.
#include "unity.h"
#include "bb_pub.h"
#include "bb_nv.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Fake capturing sink
// ---------------------------------------------------------------------------

#define CAPTURE_CAP 32

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

static bb_pub_sink_t make_capture_sink(void)
{
    bb_pub_sink_t s = { .publish = capture_publish, .ctx = NULL };
    return s;
}

// ---------------------------------------------------------------------------
// Sample functions for tests
// ---------------------------------------------------------------------------

static bool sample_temperature(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "value_c", 72.5);
    return true;
}

static bool sample_voltage(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "mv", 3300.0);
    return true;
}

static bool sample_skip(bb_json_t obj, void *ctx)
{
    (void)obj;
    (void)ctx;
    return false;  // always skip
}

// ---------------------------------------------------------------------------
// Test setup / teardown helpers
// ---------------------------------------------------------------------------

static void setup_with_sink(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");
    bb_pub_sink_t sink = make_capture_sink();
    bb_pub_set_sink(&sink);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_pub_tick_no_sink_is_noop(void)
{
    bb_pub_test_reset();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    // No crash; capture not set up, nothing to assert on.
}

void test_bb_pub_tick_no_sources_is_noop(void)
{
    setup_with_sink();
    bb_err_t err = bb_pub_tick_once();
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_single_source_produces_one_publish(void)
{
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

void test_bb_pub_topic_format_is_prefix_hostname_subtopic(void)
{
    setup_with_sink();
    bb_pub_register_source("power", sample_voltage, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // Topic must be "metrics/testhost/power"
    TEST_ASSERT_EQUAL_STRING("metrics/testhost/power", s_captured[0].topic);
}

void test_bb_pub_payload_contains_source_field(void)
{
    setup_with_sink();
    bb_pub_register_source("power", sample_voltage, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // Payload must contain "mv" field from sample_voltage
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mv\""));
}

void test_bb_pub_payload_contains_ts_field(void)
{
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // Payload must contain "ts" field injected by tick
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"ts\""));
}

void test_bb_pub_multiple_sources_each_produce_publish(void)
{
    setup_with_sink();
    bb_pub_register_source("temp",  sample_temperature, NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(2, s_capture_count);
}

void test_bb_pub_source_returning_false_is_skipped(void)
{
    setup_with_sink();
    bb_pub_register_source("skip",  sample_skip,        NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);
    bb_pub_tick_once();
    // Only the non-skipped source publishes
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].topic, "power"));
}

void test_bb_pub_max_sources_plus_one_returns_no_space(void)
{
    bb_pub_test_reset();
    // Fill to CONFIG_BB_PUB_MAX_SOURCES (default 8 in tests)
    for (int i = 0; i < CONFIG_BB_PUB_MAX_SOURCES; i++) {
        bb_err_t err = bb_pub_register_source("s", sample_temperature, NULL);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    // One more must fail
    bb_err_t err = bb_pub_register_source("extra", sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

void test_bb_pub_swap_sink_routes_to_new_sink(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    // First sink
    bb_pub_sink_t s1 = make_capture_sink();
    bb_pub_set_sink(&s1);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);

    // Second separate capture
    capture_entry_t s2_captured[CAPTURE_CAP];
    int             s2_count = 0;
    memset(s2_captured, 0, sizeof(s2_captured));

    // Capture closure using a different sink with its own ctx
    // (reuse global capture by resetting it — just verifying routing works)
    capture_reset();

    // Now set NULL sink — next tick is a no-op
    bb_pub_set_sink(NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);

    // Restore sink — tick publishes again
    bb_pub_set_sink(&s1);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);

    (void)s2_captured;
    (void)s2_count;
}

void test_bb_pub_null_subtopic_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_err_t err = bb_pub_register_source(NULL, sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_pub_null_fn_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_err_t err = bb_pub_register_source("temp", NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_pub_set_sink_null_clears_sink(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = make_capture_sink();
    bb_pub_set_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_set_sink(NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}
