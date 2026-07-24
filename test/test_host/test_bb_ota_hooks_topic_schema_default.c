// test_bb_ota_hooks_topic_schema_default -- B1-1059 SSE batch PR-3:
// config-OFF byte-identity proof for the "ota.progress" SSE topic schema.
// bb_ota_hooks_get_schema() is a plain production accessor (not gated by
// CONFIG_BB_OPENAPI_RUNTIME_META), so this runs in the plain [env:native]
// build (config OFF, the default) and asserts the served schema is the
// hand literal RELOCATED here from platform/espidf/bb_ota_hooks/bb_ota_
// hooks.c into bb_ota_hooks_wire.c -- content, not pointer (the literal is
// file-static, KB 1492).

#include "unity.h"

#include "bb_ota_hooks_wire.h"

static const char *const k_expected_ota_progress_schema =
    "{\"title\":\"OtaProgress\",\"x-sse-topic\":\"ota.progress\",\"type\":\"object\","
    "\"properties\":{"
    "\"via\":{\"type\":\"string\"},"
    "\"state\":{\"type\":\"string\","
    "\"enum\":[\"start\",\"progress\",\"success\",\"fail\",\"unknown\"]},"
    "\"pct\":{\"type\":\"integer\"}},"
    "\"required\":[\"via\",\"state\",\"pct\"]}";

void test_bb_ota_hooks_topic_schema_default_matches_relocated_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_ota_progress_schema, bb_ota_hooks_get_schema());
}
