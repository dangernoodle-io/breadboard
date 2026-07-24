// test_bb_health_stack_topic_schema_default -- B1-1059 SSE batch PR-3:
// config-OFF byte-identity proof for the "health.stack" SSE topic schema.
// bb_health_stack_get_schema() is a plain production accessor (not gated
// by CONFIG_BB_OPENAPI_RUNTIME_META), so this runs in the plain
// [env:native] build (config OFF, the default) and asserts the served
// schema is the hand literal RELOCATED here from platform/espidf/
// bb_health/bb_health_stack.c into bb_health_stack_wire.c -- content, not
// pointer (the literal is file-static, KB 1492).

#include "unity.h"

#include "bb_health_stack_wire.h"

static const char *const k_expected_health_stack_schema =
    "{\"title\":\"HealthStack\",\"x-sse-topic\":\"health.stack\",\"type\":\"object\","
    "\"properties\":{"
    "\"task\":{\"type\":\"string\"},"
    "\"free_bytes\":{\"type\":\"integer\"},"
    "\"low\":{\"type\":\"boolean\"}},"
    "\"required\":[\"task\",\"free_bytes\",\"low\"]}";

void test_bb_health_stack_topic_schema_default_matches_relocated_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_health_stack_schema, bb_health_stack_get_schema());
}
