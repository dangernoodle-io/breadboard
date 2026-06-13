// Tests for the exclusive-sink arbiter (bb_pub_exclusive_acquire/release)
// and the mutual-exclusion invariant at the section-patch and boot levels.
//
// Naming conventions follow the workspace test style.
#include "unity.h"
#include "bb_pub.h"
#include "bb_core.h"
#include "bb_mqtt_telemetry.h"
#include "bb_sink_http_telemetry.h"
#include "bb_telemetry.h"
#include "bb_nv.h"
#include "bb_json.h"
#include "bb_mqtt.h"

#include <string.h>
#include <stdbool.h>

// Section patch hooks (from host twins).
bb_err_t bb_mqtt_telemetry_section_patch_for_test(bb_json_t patch, void *ctx);
bb_err_t bb_sink_http_telemetry_section_patch_for_test(bb_json_t patch, void *ctx);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_err_t patch_mqtt(const char *body_json)
{
    bb_json_t p = bb_json_parse(body_json, 0);
    if (!p) return BB_ERR_INVALID_ARG;
    bb_err_t rc = bb_mqtt_telemetry_section_patch_for_test(p, NULL);
    bb_json_free(p);
    return rc;
}

static bb_err_t patch_http(const char *body_json)
{
    bb_json_t p = bb_json_parse(body_json, 0);
    if (!p) return BB_ERR_INVALID_ARG;
    bb_err_t rc = bb_sink_http_telemetry_section_patch_for_test(p, NULL);
    bb_json_free(p);
    return rc;
}

static void reset_all(void)
{
    // Both reset_for_test functions call bb_pub_exclusive_reset + NVS reset.
    bb_mqtt_telemetry_reset_for_test();
    // Sink HTTP reset also resets the arbiter (idempotent second call is fine).
    bb_nv_host_str_store_reset();
    bb_pub_exclusive_reset();
}

// ---------------------------------------------------------------------------
// Arbiter unit tests
// ---------------------------------------------------------------------------

void test_arbiter_acquire_free_slot_ok(void)
{
    bb_pub_exclusive_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("mqtt"));
    bb_pub_exclusive_reset();
}

void test_arbiter_acquire_same_id_idempotent(void)
{
    bb_pub_exclusive_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("mqtt"));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("mqtt"));
    bb_pub_exclusive_reset();
}

void test_arbiter_acquire_other_while_held_conflict(void)
{
    bb_pub_exclusive_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("mqtt"));
    TEST_ASSERT_EQUAL_INT(BB_ERR_CONFLICT, bb_pub_exclusive_acquire("http"));
    bb_pub_exclusive_reset();
}

void test_arbiter_release_frees_slot(void)
{
    bb_pub_exclusive_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("mqtt"));
    bb_pub_exclusive_release("mqtt");
    // After release the slot is free — the other id can now acquire.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("http"));
    bb_pub_exclusive_reset();
}

void test_arbiter_release_wrong_id_noop(void)
{
    bb_pub_exclusive_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("mqtt"));
    // Releasing with the wrong id must not free the slot.
    bb_pub_exclusive_release("http");
    TEST_ASSERT_EQUAL_INT(BB_ERR_CONFLICT, bb_pub_exclusive_acquire("http"));
    bb_pub_exclusive_reset();
}

void test_arbiter_reset_clears_slot(void)
{
    bb_pub_exclusive_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("mqtt"));
    bb_pub_exclusive_reset();
    // Slot is clear after reset — any id can acquire.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("http"));
    bb_pub_exclusive_reset();
}

// ---------------------------------------------------------------------------
// Section-level mutex tests
// ---------------------------------------------------------------------------

void test_mutex_enable_mqtt_ok_when_slot_free(void)
{
    reset_all();
    bb_err_t rc = patch_mqtt("{\"enabled\":true}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[4] = {0};
    bb_nv_get_str("bb_mqtt", "enabled", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", buf);
}

void test_mutex_enable_http_while_mqtt_active_conflict(void)
{
    reset_all();
    // Enable MQTT — acquires slot.
    TEST_ASSERT_EQUAL_INT(BB_OK, patch_mqtt("{\"enabled\":true}"));

    // Attempt to enable HTTP while MQTT holds the slot — must fail.
    bb_err_t rc = patch_http("{\"enabled\":true}");
    TEST_ASSERT_EQUAL_INT(BB_ERR_CONFLICT, rc);

    // HTTP must remain disabled in NVS.
    char buf[4] = {0};
    bb_nv_get_str("bb_sink_http", "enabled", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

void test_mutex_enable_mqtt_while_http_active_conflict(void)
{
    reset_all();
    // Enable HTTP — acquires slot.
    TEST_ASSERT_EQUAL_INT(BB_OK, patch_http("{\"enabled\":true}"));

    // Attempt to enable MQTT while HTTP holds the slot — must fail.
    bb_err_t rc = patch_mqtt("{\"enabled\":true}");
    TEST_ASSERT_EQUAL_INT(BB_ERR_CONFLICT, rc);

    // MQTT must remain disabled in NVS.
    char buf[4] = {0};
    bb_nv_get_str("bb_mqtt", "enabled", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

void test_mutex_disable_mqtt_then_enable_http_ok(void)
{
    reset_all();
    // Enable MQTT.
    TEST_ASSERT_EQUAL_INT(BB_OK, patch_mqtt("{\"enabled\":true}"));
    // Disable MQTT — releases slot.
    TEST_ASSERT_EQUAL_INT(BB_OK, patch_mqtt("{\"enabled\":false}"));

    // Now HTTP should be able to acquire.
    bb_err_t rc = patch_http("{\"enabled\":true}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    char buf[4] = {0};
    bb_nv_get_str("bb_sink_http", "enabled", buf, sizeof(buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", buf);
}

void test_mutex_patch_without_enabled_field_passes_through(void)
{
    reset_all();
    // PATCH that doesn't touch enabled must not interact with the arbiter.
    bb_err_t rc = patch_mqtt("{\"uri\":\"mqtt://broker.example.com:1883\"}");
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    // Now HTTP can still acquire the slot (not held).
    TEST_ASSERT_EQUAL_INT(BB_OK, patch_http("{\"enabled\":true}"));
}

// ---------------------------------------------------------------------------
// Boot-precedence tests
// ---------------------------------------------------------------------------

void test_boot_both_enabled_mqtt_wins(void)
{
    // Simulate both sinks enabled in NVS (invalid legacy state).
    bb_pub_exclusive_reset();
    bb_nv_host_str_store_reset();
    bb_nv_set_str("bb_mqtt", "enabled", "1");
    bb_nv_set_str("bb_sink_http", "enabled", "1");

    // MQTT init runs first (PRE_HTTP registration order).
    bb_err_t rc_mqtt = bb_mqtt_telemetry_init();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc_mqtt);

    // HTTP init runs second — finds slot taken, disables itself.
    bb_err_t rc_http = bb_sink_http_telemetry_init();
    TEST_ASSERT_EQUAL_INT(BB_OK, rc_http);  // registration still succeeds

    // MQTT must remain enabled in NVS.
    char mqtt_buf[4] = {0};
    bb_nv_get_str("bb_mqtt", "enabled", mqtt_buf, sizeof(mqtt_buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", mqtt_buf);

    // HTTP must be disabled in NVS (written back by init).
    char http_buf[4] = {0};
    bb_nv_get_str("bb_sink_http", "enabled", http_buf, sizeof(http_buf), "0");
    TEST_ASSERT_EQUAL_STRING("0", http_buf);

    // Exactly one sink holds the exclusive slot (mqtt).
    // Verify: HTTP cannot now acquire; MQTT can re-acquire idempotently.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("mqtt"));
    TEST_ASSERT_EQUAL_INT(BB_ERR_CONFLICT, bb_pub_exclusive_acquire("http"));

    // Clean up: sections registered during test; reset telemetry registry too.
    bb_telemetry_reset_for_test();
    bb_pub_exclusive_reset();
    bb_nv_host_str_store_reset();
}

void test_boot_only_mqtt_enabled(void)
{
    bb_pub_exclusive_reset();
    bb_nv_host_str_store_reset();
    bb_nv_set_str("bb_mqtt", "enabled", "1");
    // HTTP not enabled.

    bb_mqtt_telemetry_init();
    bb_sink_http_telemetry_init();

    char mqtt_buf[4] = {0};
    bb_nv_get_str("bb_mqtt", "enabled", mqtt_buf, sizeof(mqtt_buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", mqtt_buf);

    char http_buf[4] = {0};
    bb_nv_get_str("bb_sink_http", "enabled", http_buf, sizeof(http_buf), "0");
    TEST_ASSERT_EQUAL_STRING("0", http_buf);

    bb_telemetry_reset_for_test();
    bb_pub_exclusive_reset();
    bb_nv_host_str_store_reset();
}

void test_boot_only_http_enabled(void)
{
    bb_pub_exclusive_reset();
    bb_nv_host_str_store_reset();
    bb_nv_set_str("bb_sink_http", "enabled", "1");
    // MQTT not enabled.

    bb_mqtt_telemetry_init();
    bb_sink_http_telemetry_init();

    char mqtt_buf[4] = {0};
    bb_nv_get_str("bb_mqtt", "enabled", mqtt_buf, sizeof(mqtt_buf), "0");
    TEST_ASSERT_EQUAL_STRING("0", mqtt_buf);

    char http_buf[4] = {0};
    bb_nv_get_str("bb_sink_http", "enabled", http_buf, sizeof(http_buf), "0");
    TEST_ASSERT_EQUAL_STRING("1", http_buf);

    bb_telemetry_reset_for_test();
    bb_pub_exclusive_reset();
    bb_nv_host_str_store_reset();
}

void test_boot_neither_enabled(void)
{
    bb_pub_exclusive_reset();
    bb_nv_host_str_store_reset();

    bb_mqtt_telemetry_init();
    bb_sink_http_telemetry_init();

    // Neither should be enabled, slot should be free.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_pub_exclusive_acquire("any"));

    bb_telemetry_reset_for_test();
    bb_pub_exclusive_reset();
    bb_nv_host_str_store_reset();
}
