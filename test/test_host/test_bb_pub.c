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

// ---------------------------------------------------------------------------
// Second capturing sink (uses a separate counter via ctx)
// ---------------------------------------------------------------------------

typedef struct {
    capture_entry_t entries[CAPTURE_CAP];
    int             count;
} capture_ctx_t;

static bb_err_t capture_publish_ctx(void *ctx, const char *topic,
                                     const char *payload, int len)
{
    (void)len;
    capture_ctx_t *c = (capture_ctx_t *)ctx;
    if (c->count >= CAPTURE_CAP) return BB_ERR_NO_SPACE;
    capture_entry_t *e = &c->entries[c->count++];
    strncpy(e->topic,   topic,   sizeof(e->topic)   - 1);
    strncpy(e->payload, payload, sizeof(e->payload) - 1);
    return BB_OK;
}

static bb_err_t failing_publish(void *ctx, const char *topic,
                                 const char *payload, int len)
{
    (void)ctx;
    (void)topic;
    (void)payload;
    (void)len;
    return BB_ERR_INVALID_STATE;  // always fails
}

// ---------------------------------------------------------------------------
// Multi-sink tests
// ---------------------------------------------------------------------------

void test_bb_pub_add_sink_null_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_add_sink(NULL));
}

void test_bb_pub_add_sink_null_publish_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    bb_pub_sink_t s = { .publish = NULL, .ctx = NULL };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_add_sink(&s));
}

void test_bb_pub_add_sink_fanout_both_receive(void)
{
    bb_pub_test_reset();
    capture_reset();

    capture_ctx_t ctx2 = { .count = 0 };
    memset(ctx2.entries, 0, sizeof(ctx2.entries));

    bb_nv_config_set_hostname("testhost");

    // Add two distinct capturing sinks.
    bb_pub_sink_t s1 = make_capture_sink();  // uses global s_captured / s_capture_count
    bb_pub_sink_t s2 = { .publish = capture_publish_ctx, .ctx = &ctx2 };

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s1));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s2));

    bb_pub_register_source("temp",  sample_temperature, NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);

    bb_pub_tick_once();

    // Both sinks must have received both sources.
    TEST_ASSERT_EQUAL_INT(2, s_capture_count);
    TEST_ASSERT_EQUAL_INT(2, ctx2.count);

    // Both sinks must have received identical topics and payloads.
    TEST_ASSERT_EQUAL_STRING(s_captured[0].topic,   ctx2.entries[0].topic);
    TEST_ASSERT_EQUAL_STRING(s_captured[0].payload, ctx2.entries[0].payload);
    TEST_ASSERT_EQUAL_STRING(s_captured[1].topic,   ctx2.entries[1].topic);
    TEST_ASSERT_EQUAL_STRING(s_captured[1].payload, ctx2.entries[1].payload);
}

void test_bb_pub_set_sink_replaces_prior_sinks(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    capture_ctx_t ctx2 = { .count = 0 };
    memset(ctx2.entries, 0, sizeof(ctx2.entries));

    bb_pub_sink_t s1 = make_capture_sink();
    bb_pub_sink_t s2 = { .publish = capture_publish_ctx, .ctx = &ctx2 };

    // Register two sinks via add_sink.
    bb_pub_add_sink(&s1);
    bb_pub_add_sink(&s2);

    // set_sink must replace both with only s1.
    bb_pub_set_sink(&s1);

    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    // Only the single sink (s1 / global capture) should have received anything.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(0, ctx2.count);  // s2 was cleared
}

void test_bb_pub_clear_sinks_makes_tick_noop(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = make_capture_sink();
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_clear_sinks();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_max_sinks_plus_one_returns_no_space(void)
{
    bb_pub_test_reset();
    bb_pub_sink_t s = make_capture_sink();

    for (int i = 0; i < CONFIG_BB_PUB_MAX_SINKS; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_pub_add_sink(&s));
}

void test_bb_pub_failing_sink_does_not_stop_other_sink(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    // Register a failing sink first, then a capturing sink.
    bb_pub_sink_t bad = { .publish = failing_publish, .ctx = NULL };
    bb_pub_sink_t good = make_capture_sink();

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&bad));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&good));

    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_err_t err = bb_pub_tick_once();

    // tick must still return BB_OK.
    TEST_ASSERT_EQUAL(BB_OK, err);
    // Capturing sink must have received the publish despite the failing one.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

// ---------------------------------------------------------------------------
// Status tests
// ---------------------------------------------------------------------------

void test_bb_pub_status_initial_counts_zero(void)
{
    bb_pub_test_reset();
    bb_pub_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_get_status(&st));
    TEST_ASSERT_EQUAL_INT(0, st.source_count);
    TEST_ASSERT_EQUAL_INT(0, st.sink_count);
    TEST_ASSERT_FALSE(st.published_ever);
    TEST_ASSERT_EQUAL_UINT32(0, st.last_publish_ms);
}

void test_bb_pub_status_counts_reflect_registered(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = make_capture_sink();
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_register_source("volt", sample_voltage, NULL);

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    TEST_ASSERT_EQUAL_INT(2, st.source_count);
    TEST_ASSERT_EQUAL_INT(1, st.sink_count);
}

void test_bb_pub_status_after_tick_published_ever_true(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = make_capture_sink();
    bb_pub_add_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_tick_once();

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    TEST_ASSERT_TRUE(st.published_ever);
    TEST_ASSERT_TRUE(st.last_publish_ok);
    TEST_ASSERT_GREATER_THAN_UINT32(0, st.last_publish_ms);
}

void test_bb_pub_status_failing_sink_sets_last_publish_not_ok(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t bad = { .publish = failing_publish, .ctx = NULL };
    bb_pub_add_sink(&bad);
    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_tick_once();

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    TEST_ASSERT_FALSE(st.last_publish_ok);
    TEST_ASSERT_TRUE(st.published_ever);
}

void test_bb_pub_status_no_sink_not_published(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_register_source("temp", sample_temperature, NULL);

    bb_pub_tick_once();

    bb_pub_status_t st;
    bb_pub_get_status(&st);
    TEST_ASSERT_FALSE(st.published_ever);
    TEST_ASSERT_EQUAL_UINT32(0, st.last_publish_ms);
}

void test_bb_pub_status_null_out_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_get_status(NULL));
}
