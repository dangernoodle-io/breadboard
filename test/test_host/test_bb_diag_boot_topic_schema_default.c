// test_bb_diag_boot_topic_schema_default -- B1-1059 SSE batch PR-3:
// config-OFF byte-identity proof for the "diag.boot" SSE topic schema.
// bb_diag_boot_get_schema() is a plain production accessor (not gated by
// CONFIG_BB_OPENAPI_RUNTIME_META), so this runs in the plain [env:native]
// build (config OFF, the default) and asserts the served schema is the
// hand literal RELOCATED here from platform/espidf/bb_diag_http/bb_diag_
// http_routes.c into bb_diag_boot_wire.c -- content, not pointer (the
// literal is file-static, KB 1492).

#include "unity.h"

#include "bb_diag_boot_wire.h"

static const char *const k_expected_diag_boot_schema =
    "{\"title\":\"DiagBoot\",\"x-sse-topic\":\"diag.boot\",\"type\":\"object\","
    "\"properties\":{"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"wdt_resets\":{\"type\":\"integer\"},"
    "\"panic\":{\"type\":\"object\",\"properties\":{"
    "\"available\":{\"type\":\"boolean\"},"
    "\"boots_since\":{\"type\":\"integer\"}}},"
    "\"pending_verify\":{\"type\":\"boolean\"},"
    "\"rolled_back\":{\"type\":\"boolean\"},"
    "\"reboot_reason\":{\"type\":\"object\",\"properties\":{"
    "\"source\":{\"type\":\"string\"},"
    "\"detail\":{\"type\":\"string\"},"
    "\"uptime_s\":{\"type\":\"integer\"},"
    "\"epoch_s\":{\"type\":\"integer\"},"
    "\"age_s\":{\"type\":\"integer\"}}},"
    "\"reboot_history\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"source\":{\"type\":\"string\"},"
    "\"epoch_s\":{\"type\":\"integer\"},"
    "\"uptime_s\":{\"type\":\"integer\"}}}}},"
    "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\","
    "\"pending_verify\",\"rolled_back\",\"reboot_reason\",\"reboot_history\"]}";

void test_bb_diag_boot_topic_schema_default_matches_relocated_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_boot_schema, bb_diag_boot_get_schema());
}
