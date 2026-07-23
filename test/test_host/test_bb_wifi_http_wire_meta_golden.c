// test_bb_wifi_http_wire_meta_golden -- B1-1059 PR-2a exemplar: proves the
// #if-gated co-located bb_serialize_desc_meta_t pattern end-to-end for
// GET /api/wifi (bb_wifi_http_info_wire_desc /
// bb_wifi_http_info_wire_meta, both in
// components/bb_wifi_http/bb_wifi_http_wire.c). Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_wifi_http_info_wire_meta doesn't exist there.
//
// Fidelity finding vs the hand-authored k_wifi_info_schema literal
// (platform/espidf/bb_wifi_http/bb_wifi_http_routes.c): the per-field
// "properties" content and the "required" set are BYTE-IDENTICAL (same
// field order, same JSON Schema "type" per field, same two required
// keys: ssid, connected). Two structural differences are inherent to the
// meta engine's fixed schema-shape policy, not this table's content, and
// are accepted as documented deltas (not a fidelity failure):
//   1. no top-level "title" -- bb_serialize_meta_openapi_schema() has no
//      descriptor-level title/description hook (only per-FIELD title/
//      description via bb_serialize_field_meta_t); the literal's
//      "title":"WifiInfo" is supplied instead via the OpenAPI component
//      name callers already pass to bb_openapi_register_schema().
//   2. a trailing "additionalProperties":false -- the engine always
//      closes every rendered object (same policy proven by every other
//      golden in test_bb_serialize_meta_openapi.c); the hand literal
//      predates that policy and simply never set it.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_wifi_http/bb_wifi_http_wire_priv.h"

#include <string.h>

// Byte-fidelity target: k_wifi_info_schema's "properties"/"required"
// content, re-expressed as bb_serialize_meta_openapi_schema()'s fixed
// object-schema shape (see file banner for the two documented deltas).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
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
    "\"required\":[\"ssid\",\"connected\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting
// the table enough to render a schema from it.
void test_bb_wifi_http_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_wifi_http_info_wire_desc, &bb_wifi_http_info_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces k_wifi_info_schema's field-level
// content exactly, modulo the two documented structural deltas above.
void test_bb_wifi_http_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_wifi_http_info_wire_desc, &bb_wifi_http_info_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
