// test_bb_wifi_http_wire_schema_default -- B1-1059 emit batch B, site B1:
// config-OFF byte-identity proof for GET /api/wifi's response schema.
// bb_wifi_http_info_wire_get_schema() is a plain production accessor (not
// gated by CONFIG_BB_OPENAPI_RUNTIME_META), so this runs in the plain
// [env:native] build (config OFF, the default) and asserts the served
// schema is the hand literal RELOCATED here from platform/espidf/
// bb_wifi_http/bb_wifi_http_routes.c into bb_wifi_http_wire.c -- content,
// not pointer (the literal is file-static, KB 1492). This is the "live
// route/register schema" surrogate for this cross-TU site: bb_wifi_routes_
// init() (ESP-IDF-only, cannot link on host) always serves exactly what
// this accessor returns.

#include "unity.h"

#include "../../components/bb_wifi_http/bb_wifi_http_wire_priv.h"

static const char *const k_expected_wifi_info_schema =
    "{\"title\":\"WifiInfo\",\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"string\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
    "\"restart_sta_count\":{\"type\":\"integer\"},"
    "\"disconnect_rssi\":{\"type\":\"integer\"}},"
    "\"required\":[\"ssid\",\"connected\"]}";

void test_bb_wifi_http_wire_schema_default_matches_relocated_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_wifi_info_schema, bb_wifi_http_info_wire_get_schema());
}
