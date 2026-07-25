#include "unity.h"
#include "bb_health_stack_wire.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves the "health.stack" SSE topic schema's runtime-compose path
// (B1-1059 SSE batch PR-3) actually wires up: bb_health_stack_ensure_
// schema_patched() runs exactly once and composes content byte-identical
// to bb_serialize_meta_openapi_schema()'s own proven output over bb_health_
// stack_wire_desc/_meta (already golden-tested against the hand literal by
// test_bb_health_stack_wire_meta_golden.c), and re-running it is pointer-
// stable (idempotent -- never re-composes once patched).

// Full expected composed schema: bb_serialize_meta_openapi_topic_schema()'s
// `{"title":"HealthStack","x-sse-topic":"health.stack",` prefix ahead of
// the SAME body test_bb_health_stack_wire_meta_golden.c's k_expected_meta_
// schema proves bb_serialize_meta_openapi_schema() renders for bb_health_
// stack_wire_desc/_meta (properties/required content byte-identical to the
// pre-existing hand literal, PLUS the trailing "additionalProperties":false
// delta).
static const char *const k_expected_health_stack_schema =
    "{\"title\":\"HealthStack\",\"x-sse-topic\":\"health.stack\","
    "\"type\":\"object\",\"properties\":{"
    "\"task\":{\"type\":\"string\"},"
    "\"free_bytes\":{\"type\":\"integer\"},"
    "\"low\":{\"type\":\"boolean\"}},"
    "\"required\":[\"task\",\"free_bytes\",\"low\"],"
    "\"additionalProperties\":false}";

// Exercises bb_health_stack_ensure_schema_patched()'s own failure contract
// directly (not via bb_health_stack_monitor_init(), which is ESP_PLATFORM-
// gated and not host-testable): forces the engine (bb_serialize_meta, via
// BB_SERIALIZE_META_TESTING's fail-injection seam) to return
// BB_ERR_NO_SPACE and asserts the error propagates with the schema buffer
// left unpatched (empty) -- the invariant bb_health_stack_monitor_init()'s
// CONFIG_BB_OPENAPI_RUNTIME_META guard relies on to skip registration on
// a compose failure. MUST run before the two success tests below: the
// compose-and-patch step is guarded/idempotent (a non-empty schema buffer
// short-circuits a second real compose), so once a prior test has
// successfully composed it this seam can no longer force a re-compose --
// see test_main.c's RUN_TEST order.
void test_bb_health_stack_topic_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_health_stack_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_health_stack_get_schema());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_health_stack_topic_schema_matches_expected_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_health_stack_ensure_schema_patched());

    const char *schema = bb_health_stack_get_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_health_stack_schema, schema);
}

void test_bb_health_stack_topic_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_health_stack_get_schema();
    TEST_ASSERT_EQUAL_STRING(k_expected_health_stack_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_health_stack_ensure_schema_patched();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_health_stack_get_schema();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_health_stack_schema, second);
}
