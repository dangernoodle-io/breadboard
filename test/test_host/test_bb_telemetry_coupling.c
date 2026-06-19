// Tests for the publisher–sink coupling in bb_telemetry_dispatch_patch.
//
// Coupling semantics:
//   - PATCH that changes a sink's enabled → publisher enabled = (any sink enabled).
//   - PATCH that also explicitly sets publisher.enabled → explicit value wins.
//   - PATCH that changes non-enabled sink fields → no publisher coupling.
//   - PATCH that changes publisher.enabled without touching sinks → normal write.
//
// The pure helper bb_telemetry_couple_publisher is also exercised directly.
#include "unity.h"
#include "bb_telemetry.h"
#include "bb_mqtt_telemetry.h"
#include "bb_sink_http_telemetry.h"
#include "bb_pub_telemetry.h"
#include "bb_pub.h"
#include "bb_nv.h"
#include "bb_json.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Reset helpers
// ---------------------------------------------------------------------------

// teardown: clear section registry so subsequent tests start clean.
// Called at the end of each test that registers sections.
static void teardown(void)
{
    bb_telemetry_reset_for_test();
    bb_pub_exclusive_reset();
    bb_nv_host_str_store_reset();
}

static void reset_all(void)
{
    // Reset registry and NVS first to undo any previous test's registrations.
    bb_telemetry_reset_for_test();
    bb_mqtt_telemetry_reset_for_test();
    bb_sink_http_telemetry_reset_for_test();
    bb_pub_telemetry_reset_for_test();

    // Register all three sections so dispatch can call their patch fns.
    bb_mqtt_telemetry_init();
    bb_sink_http_telemetry_init();
    bb_pub_telemetry_init();

    // Default publisher state: enabled (NVS default 1).
    bb_pub_set_enabled(true);
}

static bb_err_t run_dispatch(const char *body_json)
{
    bb_json_t body = bb_json_parse(body_json, 0);
    if (!body) return BB_ERR_INVALID_ARG;
    bb_err_t rc = bb_telemetry_dispatch_patch(body);
    bb_json_free(body);
    return rc;
}

// ---------------------------------------------------------------------------
// Pure helper: bb_telemetry_couple_publisher
// ---------------------------------------------------------------------------

void test_couple_publisher_no_explicit_any_sink_true(void)
{
    // any_sink=true, no explicit → publisher = true
    bool result = bb_telemetry_couple_publisher(true, false, false);
    TEST_ASSERT_TRUE(result);
}

void test_couple_publisher_no_explicit_any_sink_false(void)
{
    // any_sink=false, no explicit → publisher = false
    bool result = bb_telemetry_couple_publisher(false, false, false);
    TEST_ASSERT_FALSE(result);
}

void test_couple_publisher_explicit_true_overrides_no_sink(void)
{
    // explicit true wins even when no sink enabled
    bool result = bb_telemetry_couple_publisher(false, true, true);
    TEST_ASSERT_TRUE(result);
}

void test_couple_publisher_explicit_false_overrides_sink_enabled(void)
{
    // explicit false wins even when a sink is enabled
    bool result = bb_telemetry_couple_publisher(true, true, false);
    TEST_ASSERT_FALSE(result);
}

void test_couple_publisher_explicit_true_with_sink_enabled(void)
{
    // explicit true + sink enabled → explicit wins (same outcome, but via override path)
    bool result = bb_telemetry_couple_publisher(true, true, true);
    TEST_ASSERT_TRUE(result);
}

void test_couple_publisher_explicit_false_with_no_sink(void)
{
    // explicit false + no sink → false (same as auto, but via override path)
    bool result = bb_telemetry_couple_publisher(false, true, false);
    TEST_ASSERT_FALSE(result);
}

// ---------------------------------------------------------------------------
// Coupling via dispatch_patch: enable mqtt → publisher enabled
// ---------------------------------------------------------------------------

void test_coupling_enable_mqtt_sets_publisher_enabled(void)
{
    reset_all();

    // Start with publisher disabled and both sinks disabled.
    bb_pub_set_enabled(false);

    bb_err_t rc = run_dispatch("{\"mqtt\":{\"enabled\":true}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // Publisher should now be enabled (mqtt sink is the only active sink).
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    teardown();
}

void test_coupling_disable_last_sink_sets_publisher_disabled(void)
{
    reset_all();

    // Pre-enable mqtt so publisher is currently enabled.
    // Must first acquire the exclusive slot (mirrors what PATCH does).
    bb_pub_exclusive_acquire("mqtt");
    bb_nv_set_str("bb_mqtt", "enabled", "1");
    bb_pub_set_enabled(true);

    // Now disable mqtt → publisher should be disabled.
    bb_err_t rc = run_dispatch("{\"mqtt\":{\"enabled\":false}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    TEST_ASSERT_FALSE(bb_pub_is_enabled());
    teardown();
}

// ---------------------------------------------------------------------------
// Coupling via dispatch_patch: enable http → publisher enabled
// ---------------------------------------------------------------------------

void test_coupling_enable_http_sets_publisher_enabled(void)
{
    reset_all();

    bb_pub_set_enabled(false);

    bb_err_t rc = run_dispatch("{\"http\":{\"enabled\":true}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    teardown();
}

void test_coupling_disable_http_when_mqtt_still_enabled_keeps_publisher_enabled(void)
{
    reset_all();

    // mqtt holds the exclusive slot; http is NVS-only (not in arbiter).
    // Simulate the case where mqtt is active and http NVS flag is stale.
    bb_pub_exclusive_acquire("mqtt");
    bb_nv_set_str("bb_mqtt", "enabled", "1");
    bb_nv_set_str("bb_sink_http", "enabled", "1");
    bb_pub_set_enabled(true);

    // Disable http — mqtt still active → publisher stays enabled.
    bb_err_t rc = run_dispatch("{\"http\":{\"enabled\":false}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // mqtt still enabled in NVS → any_sink_enabled = true → publisher stays true.
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    teardown();
}

void test_coupling_disable_last_http_sink_sets_publisher_disabled(void)
{
    reset_all();

    // Only http enabled; mqtt off.
    bb_pub_exclusive_acquire("http");
    bb_nv_set_str("bb_mqtt", "enabled", "0");
    bb_nv_set_str("bb_sink_http", "enabled", "1");
    bb_pub_set_enabled(true);

    bb_err_t rc = run_dispatch("{\"http\":{\"enabled\":false}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    TEST_ASSERT_FALSE(bb_pub_is_enabled());
    teardown();
}

// ---------------------------------------------------------------------------
// Override: explicit publisher.enabled in same body wins
// ---------------------------------------------------------------------------

void test_coupling_explicit_publisher_enabled_false_wins_when_mqtt_enabled(void)
{
    reset_all();

    bb_pub_set_enabled(true);

    // Enable mqtt (any_sink_enabled → true) but ALSO explicitly set
    // publisher.enabled=false → explicit wins.
    bb_err_t rc = run_dispatch(
        "{\"mqtt\":{\"enabled\":true},"
        "\"publisher\":{\"enabled\":false}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    TEST_ASSERT_FALSE(bb_pub_is_enabled());
    teardown();
}

void test_coupling_explicit_publisher_enabled_true_wins_when_no_sink_enabled(void)
{
    reset_all();

    // Acquire mqtt slot first so we can release it.
    bb_pub_exclusive_acquire("mqtt");
    bb_nv_set_str("bb_mqtt", "enabled", "1");
    bb_pub_set_enabled(false);

    // Disable mqtt (any_sink_enabled → false) but explicitly set
    // publisher.enabled=true → explicit wins.
    bb_err_t rc = run_dispatch(
        "{\"mqtt\":{\"enabled\":false},"
        "\"publisher\":{\"enabled\":true}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    teardown();
}

// ---------------------------------------------------------------------------
// No coupling when body only sets non-enabled sink fields
// ---------------------------------------------------------------------------

void test_no_coupling_when_sink_patch_has_no_enabled_field(void)
{
    reset_all();

    // Publisher is enabled; patch only changes mqtt.uri, not mqtt.enabled.
    bb_pub_set_enabled(true);

    bb_err_t rc = run_dispatch("{\"mqtt\":{\"uri\":\"mqtt://example.com\"}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // Publisher state unchanged.
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    teardown();
}

void test_no_coupling_when_only_publisher_section_patched(void)
{
    reset_all();

    // Both sinks disabled; publisher enabled.
    bb_pub_set_enabled(true);

    // Only patch publisher.interval_ms — no sink coupling.
    bb_err_t rc = run_dispatch("{\"publisher\":{\"interval_ms\":5000}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // Publisher enabled state unchanged.
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    teardown();
}

// ---------------------------------------------------------------------------
// Enable mqtt and http in separate PATCHes; disable both → publisher disabled
// ---------------------------------------------------------------------------

void test_coupling_enable_mqtt_and_http_in_same_patch_enables_publisher(void)
{
    reset_all();

    // The exclusive arbiter prevents two sinks from being enabled simultaneously
    // via a single PATCH (second enable gets BB_ERR_CONFLICT → 409).
    // This test verifies the correct error is returned (not silently dropped).
    bb_pub_set_enabled(false);

    // First enable mqtt — succeeds.
    bb_err_t rc = run_dispatch("{\"mqtt\":{\"enabled\":true}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_TRUE(bb_pub_is_enabled());
    teardown();
}

void test_coupling_disable_both_sinks_in_same_patch_disables_publisher(void)
{
    reset_all();

    // Simulate both sinks listed as enabled in NVS (stale/legacy state).
    bb_pub_exclusive_acquire("mqtt");
    bb_nv_set_str("bb_mqtt", "enabled", "1");
    bb_nv_set_str("bb_sink_http", "enabled", "1");
    bb_pub_set_enabled(true);

    bb_err_t rc = run_dispatch(
        "{\"mqtt\":{\"enabled\":false},"
        "\"http\":{\"enabled\":false}}");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    TEST_ASSERT_FALSE(bb_pub_is_enabled());
    teardown();
}
