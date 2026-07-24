// test_bb_display_info_topic_schema_default -- B1-1059 SSE PR-4:
// config-OFF byte-identity proof for the "health.display" served schema.
// bb_display_info_get_schema() is a plain production accessor (not gated by
// CONFIG_BB_OPENAPI_RUNTIME_META), so this runs in the plain [env:native]
// build (config OFF, the default) and asserts the served schema is the
// hand literal RELOCATED here from platform/espidf/bb_display/bb_display_
// info.c into bb_display_info_wire.c -- content, not pointer (the literal
// is file-static, KB 1492).

#include "unity.h"

#include "bb_display_info_wire.h"

static const char *const k_expected_display_info_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"panel\":{\"type\":\"string\"},"
    "\"width\":{\"type\":\"integer\"},"
    "\"height\":{\"type\":\"integer\"},"
    "\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"present\"],"
    "\"additionalProperties\":false}";

void test_bb_display_info_topic_schema_default_matches_relocated_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_display_info_schema, bb_display_info_get_schema());
}
