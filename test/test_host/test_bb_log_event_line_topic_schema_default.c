// test_bb_log_event_line_topic_schema_default -- B1-1059 SSE batch PR-3:
// config-OFF byte-identity proof for the "log" SSE topic schema. bb_log_
// event_line_get_schema() is a plain production accessor (not gated by
// CONFIG_BB_OPENAPI_RUNTIME_META), so this runs in the plain [env:native]
// build (config OFF, the default) and asserts the served schema is the
// hand literal RELOCATED here from platform/espidf/bb_log_event/bb_log_
// event.c into bb_log_event_line_wire.c -- content, not pointer (the
// literal is file-static, KB 1492).

#include "unity.h"

#include "../../components/bb_log_event/bb_log_event_line_wire_priv.h"

static const char *const k_expected_log_event_schema =
    "{\"title\":\"LogEvent\",\"x-sse-topic\":\"log\",\"type\":\"object\","
    "\"properties\":{"
    "\"ts\":{\"type\":\"integer\"},"
    "\"level\":{\"type\":\"string\",\"enum\":[\"I\",\"W\",\"E\",\"D\",\"V\",\"?\"]},"
    "\"tag\":{\"type\":\"string\"},"
    "\"msg\":{\"type\":\"string\"}},"
    "\"required\":[\"ts\",\"level\",\"tag\",\"msg\"]}";

void test_bb_log_event_line_topic_schema_default_matches_relocated_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_log_event_schema, bb_log_event_line_get_schema());
}
