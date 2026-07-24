// test_bb_wifi_http_scan_wire_schema_default -- B1-1059 emit batch B, site
// B2: config-OFF byte-identity proof for POST /api/wifi/scan's response
// schema. bb_wifi_http_scan_wire_get_schema() is a plain production
// accessor (not gated by CONFIG_BB_OPENAPI_RUNTIME_META), so this runs in
// the plain [env:native] build (config OFF, the default) and asserts the
// served schema is BB_WIFI_HTTP_SCAN_SCHEMA_LITERAL -- the SAME macro text
// platform/espidf/bb_wifi_http/bb_wifi_http_routes.c's config-OFF
// s_scan_responses[0].schema uses directly (that TU is ESP-IDF-only and
// cannot link on host; this is its "live route/register schema" surrogate,
// content not pointer, KB 1492).

#include "unity.h"

#include "../../components/bb_wifi_http/bb_wifi_http_scan_wire_priv.h"

static const char *const k_expected_wifi_scan_schema = BB_WIFI_HTTP_SCAN_SCHEMA_LITERAL;

void test_bb_wifi_http_scan_wire_schema_default_matches_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_wifi_scan_schema, bb_wifi_http_scan_wire_get_schema());
}
