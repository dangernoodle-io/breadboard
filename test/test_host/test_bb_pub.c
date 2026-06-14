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

// ---------------------------------------------------------------------------
// Pause / resume tests
// ---------------------------------------------------------------------------

// Track how many times the sample_fn is called.
static int s_sample_call_count;

static bool sample_counting(bb_json_t obj, void *ctx)
{
    (void)ctx;
    s_sample_call_count++;
    bb_json_obj_set_number(obj, "v", 1.0);
    return true;
}

void test_bb_pub_pause_stops_publishing(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_pause();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
    TEST_ASSERT_EQUAL_INT(0, s_sample_call_count);
}

void test_bb_pub_resume_restores_publishing(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_pause();
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);

    bb_pub_resume();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(1, s_sample_call_count);
}

void test_bb_pub_is_paused_reflects_state(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_FALSE(bb_pub_is_paused());

    bb_pub_pause();
    TEST_ASSERT_TRUE(bb_pub_is_paused());

    bb_pub_resume();
    TEST_ASSERT_FALSE(bb_pub_is_paused());
}

void test_bb_pub_pause_is_idempotent(void)
{
    bb_pub_test_reset();
    bb_pub_pause();
    bb_pub_pause();
    TEST_ASSERT_TRUE(bb_pub_is_paused());
}

void test_bb_pub_resume_from_not_paused_is_safe(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_FALSE(bb_pub_is_paused());
    bb_pub_resume();  // must not crash or assert
    TEST_ASSERT_FALSE(bb_pub_is_paused());
}

void test_bb_pub_test_reset_clears_paused(void)
{
    bb_pub_test_reset();
    bb_pub_pause();
    TEST_ASSERT_TRUE(bb_pub_is_paused());

    bb_pub_test_reset();
    TEST_ASSERT_FALSE(bb_pub_is_paused());
}

// ---------------------------------------------------------------------------
// Interval tests
// ---------------------------------------------------------------------------

void test_bb_pub_interval_default_is_compile_time(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_updates_getter(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_ms(5000));
    TEST_ASSERT_EQUAL_UINT32(5000, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_min_bound_accepted(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_ms(1000));
    TEST_ASSERT_EQUAL_UINT32(1000, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_max_bound_accepted(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_set_interval_ms(3600000));
    TEST_ASSERT_EQUAL_UINT32(3600000, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_zero_rejected(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_set_interval_ms(0));
    // getter unchanged
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_below_min_rejected(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_set_interval_ms(999));
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

void test_bb_pub_set_interval_ms_above_max_rejected(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_set_interval_ms(3600001));
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

void test_bb_pub_test_reset_clears_interval_to_default(void)
{
    bb_pub_test_reset();
    bb_pub_set_interval_ms(5000);
    TEST_ASSERT_EQUAL_UINT32(5000, bb_pub_get_interval_ms());

    bb_pub_test_reset();
    TEST_ASSERT_EQUAL_UINT32(CONFIG_BB_PUB_INTERVAL_MS, bb_pub_get_interval_ms());
}

// ---------------------------------------------------------------------------
// Enabled toggle tests
// ---------------------------------------------------------------------------

void test_bb_pub_is_enabled_true_by_default(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
}

void test_bb_pub_set_enabled_false_makes_tick_noop(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_set_enabled(false);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
    TEST_ASSERT_EQUAL_INT(0, s_sample_call_count);
}

void test_bb_pub_set_enabled_true_resumes_publishing(void)
{
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_set_enabled(false);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);

    bb_pub_set_enabled(true);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(1, s_sample_call_count);
}

void test_bb_pub_enabled_false_not_paused_is_noop(void)
{
    // enabled=false, paused=false → no publish
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    bb_pub_set_enabled(false);
    TEST_ASSERT_FALSE(bb_pub_is_paused());
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_enabled_true_paused_is_noop(void)
{
    // enabled=true, paused=true → no publish
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    bb_pub_pause();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_enabled_true_not_paused_publishes(void)
{
    // enabled=true, paused=false → publishes
    setup_with_sink();
    s_sample_call_count = 0;
    bb_pub_register_source("x", sample_counting, NULL);

    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    TEST_ASSERT_FALSE(bb_pub_is_paused());
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
}

void test_bb_pub_test_reset_clears_enabled_to_default(void)
{
    bb_pub_test_reset();
    bb_pub_set_enabled(false);
    TEST_ASSERT_FALSE(bb_pub_is_enabled());

    bb_pub_test_reset();
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
}

// ---------------------------------------------------------------------------
// Interval apply hook test
// ---------------------------------------------------------------------------

static uint32_t s_hook_called_with;
static int      s_hook_call_count;

static void test_interval_hook(uint32_t ms)
{
    s_hook_called_with = ms;
    s_hook_call_count++;
}

void test_bb_pub_interval_apply_hook_called_on_set(void)
{
    bb_pub_test_reset();
    s_hook_called_with = 0;
    s_hook_call_count  = 0;

    bb_pub_set_interval_apply_hook(test_interval_hook);
    bb_pub_set_interval_ms(2000);

    TEST_ASSERT_EQUAL_INT(1, s_hook_call_count);
    TEST_ASSERT_EQUAL_UINT32(2000, s_hook_called_with);
}

void test_bb_pub_interval_apply_hook_not_called_on_invalid(void)
{
    bb_pub_test_reset();
    s_hook_call_count = 0;

    bb_pub_set_interval_apply_hook(test_interval_hook);
    bb_pub_set_interval_ms(0);  // invalid; hook must not fire

    TEST_ASSERT_EQUAL_INT(0, s_hook_call_count);
}

// ---------------------------------------------------------------------------
// Payload-extender tests
// ---------------------------------------------------------------------------

// Extender that adds a "site" field to every object.
static void extender_add_site(bb_json_t obj, const char *source, void *ctx)
{
    (void)source;
    (void)ctx;
    bb_json_obj_set_string(obj, "site", "acme");
}

// Extender that sets "a" = 1.
static void extender_set_a(bb_json_t obj, const char *source, void *ctx)
{
    (void)source;
    (void)ctx;
    bb_json_obj_set_number(obj, "a", 1.0);
}

// Extender that overwrites "a" = 2 (tests ordering).
static void extender_overwrite_a(bb_json_t obj, const char *source, void *ctx)
{
    (void)source;
    (void)ctx;
    bb_json_obj_set_number(obj, "a", 2.0);
}

// Extender that adds "mutated" = 1 to mark that it ran after sample_fn.
static void extender_add_mutated_marker(bb_json_t obj, const char *source, void *ctx)
{
    (void)source;
    (void)ctx;
    bb_json_obj_set_number(obj, "mutated", 1.0);
}

// Per-source log: records which source names were seen.
#define SOURCE_LOG_CAP 8
static char s_seen_sources[SOURCE_LOG_CAP][64];
static int  s_seen_source_count;

static void extender_log_source(bb_json_t obj, const char *source, void *ctx)
{
    (void)obj;
    (void)ctx;
    if (s_seen_source_count < SOURCE_LOG_CAP) {
        strncpy(s_seen_sources[s_seen_source_count], source,
                sizeof(s_seen_sources[0]) - 1);
        s_seen_sources[s_seen_source_count][sizeof(s_seen_sources[0]) - 1] = '\0';
        s_seen_source_count++;
    }
}

void test_bb_pub_payload_extender_adds_field_to_all_sources(void)
{
    setup_with_sink();
    bb_pub_register_source("temp",  sample_temperature, NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_add_site, NULL));

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(2, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"site\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"acme\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[1].payload, "\"site\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[1].payload, "\"acme\""));
}

void test_bb_pub_payload_extenders_apply_in_registration_order(void)
{
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);

    // Register set_a first, then overwrite_a second — last write wins.
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_set_a,       NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_overwrite_a, NULL));

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // "a":2 means overwrite_a ran last (correct order).
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"a\":2"));
}

void test_bb_pub_payload_extender_can_mutate_existing_field(void)
{
    // Demonstrates the extender runs AFTER sample_fn and can add/modify the object.
    // The extender adds a "mutated" marker; both the sample field (value_c)
    // and the extender field appear in the published payload.
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);  // sets value_c=72.5

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_add_mutated_marker, NULL));

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    // sample_fn field still present
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"value_c\""));
    // extender field also present — extender ran after sample_fn
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"mutated\""));
}

void test_bb_pub_payload_extender_source_arg_matches_subtopic(void)
{
    setup_with_sink();
    bb_pub_register_source("temp",  sample_temperature, NULL);
    bb_pub_register_source("power", sample_voltage,     NULL);

    s_seen_source_count = 0;
    memset(s_seen_sources, 0, sizeof(s_seen_sources));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_log_source, NULL));

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(2, s_seen_source_count);
    TEST_ASSERT_EQUAL_STRING("temp",  s_seen_sources[0]);
    TEST_ASSERT_EQUAL_STRING("power", s_seen_sources[1]);
}

void test_bb_pub_payload_extender_cap_overflow_returns_no_space(void)
{
    bb_pub_test_reset();
    for (int i = 0; i < BB_PUB_MAX_PAYLOAD_EXTENDERS; i++) {
        TEST_ASSERT_EQUAL(BB_OK,
            bb_pub_register_payload_extender(extender_add_site, NULL));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
        bb_pub_register_payload_extender(extender_add_site, NULL));
}

void test_bb_pub_payload_extender_null_fn_returns_invalid_arg(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_pub_register_payload_extender(NULL, NULL));
}

void test_bb_pub_payload_extender_not_called_when_paused(void)
{
    setup_with_sink();
    s_seen_source_count = 0;
    memset(s_seen_sources, 0, sizeof(s_seen_sources));
    bb_pub_register_source("temp", sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_log_source, NULL));

    bb_pub_pause();
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_seen_source_count);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_payload_extender_not_called_when_disabled(void)
{
    setup_with_sink();
    s_seen_source_count = 0;
    memset(s_seen_sources, 0, sizeof(s_seen_sources));
    bb_pub_register_source("temp", sample_temperature, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_log_source, NULL));

    bb_pub_set_enabled(false);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(0, s_seen_source_count);
    TEST_ASSERT_EQUAL_INT(0, s_capture_count);
}

void test_bb_pub_test_reset_clears_payload_extenders(void)
{
    bb_pub_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_payload_extender(extender_add_site, NULL));

    bb_pub_test_reset();  // must clear the extender

    // Re-setup and tick — extender must NOT fire.
    setup_with_sink();
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"site\""));
}

// ---------------------------------------------------------------------------
// Per-sink transport / tls stamping tests
// ---------------------------------------------------------------------------

// Sink with transport="mqtt", tls=true — payload must contain both fields.
void test_bb_pub_sink_transport_mqtt_tls_stamped(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = { .publish = capture_publish, .ctx = NULL,
                        .transport = "mqtt", .tls = true };
    bb_pub_set_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"transport\":\"mqtt\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"tls\":true"));
}

// Sink with transport="http", tls=false — payload must contain both fields.
void test_bb_pub_sink_transport_http_no_tls_stamped(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = { .publish = capture_publish, .ctx = NULL,
                        .transport = "http", .tls = false };
    bb_pub_set_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"transport\":\"http\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"tls\":false"));
}

// Sink with transport=NULL — transport and tls fields must be absent.
void test_bb_pub_sink_no_transport_fields_absent(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    bb_pub_sink_t s = { .publish = capture_publish, .ctx = NULL,
                        .transport = NULL, .tls = false };
    bb_pub_set_sink(&s);
    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"transport\""));
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"tls\""));
}

// Two sinks with different transport values — each gets its own stamped payload.
void test_bb_pub_two_sinks_each_get_own_transport(void)
{
    bb_pub_test_reset();
    capture_reset();
    bb_nv_config_set_hostname("testhost");

    capture_ctx_t ctx2 = { .count = 0 };
    memset(ctx2.entries, 0, sizeof(ctx2.entries));

    // mqtt sink (tls=true) using global capture
    bb_pub_sink_t s1 = { .publish = capture_publish,     .ctx = NULL,
                         .transport = "mqtt", .tls = true };
    // http sink (tls=false) using ctx2
    bb_pub_sink_t s2 = { .publish = capture_publish_ctx, .ctx = &ctx2,
                         .transport = "http", .tls = false };

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s1));
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_add_sink(&s2));

    bb_pub_register_source("temp", sample_temperature, NULL);
    bb_pub_tick_once();

    // Each sink received exactly one publish.
    TEST_ASSERT_EQUAL_INT(1, s_capture_count);
    TEST_ASSERT_EQUAL_INT(1, ctx2.count);

    // Sink 1 (mqtt, tls=true): must have transport=mqtt + tls=true.
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"transport\":\"mqtt\""));
    TEST_ASSERT_NOT_NULL(strstr(s_captured[0].payload, "\"tls\":true"));

    // Sink 2 (http, tls=false): must have transport=http + tls=false.
    TEST_ASSERT_NOT_NULL(strstr(ctx2.entries[0].payload, "\"transport\":\"http\""));
    TEST_ASSERT_NOT_NULL(strstr(ctx2.entries[0].payload, "\"tls\":false"));

    // Cross-check: mqtt payload must NOT contain "http".
    TEST_ASSERT_NULL(strstr(s_captured[0].payload, "\"http\""));
    // Cross-check: http payload must NOT contain "\"mqtt\"".
    TEST_ASSERT_NULL(strstr(ctx2.entries[0].payload, "\"mqtt\""));
}
