// test_bb_wifi_http_scan_wire_meta_golden -- B1-1059 PR-3 prework: proves the
// #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a
// exemplar, test_bb_wifi_http_wire_meta_golden.c) for the POST
// /api/wifi/scan RESPONSE descriptor (bb_wifi_http_scan_wire_desc /
// bb_wifi_http_scan_wire_meta, both in
// components/bb_wifi_http/bb_wifi_http_scan_wire.c -- the hand-authored
// schema stays inline in platform/espidf/bb_wifi_http/bb_wifi_http_routes.c
// (s_scan_responses[0].schema), which is ESP-IDF-only and could not host a
// meta table -- same split as test_bb_wifi_http_creds_wire_meta_golden.c's
// precedent). Only reachable when BB_SERIALIZE_META_HOST is defined (this
// native env; see platformio.ini) -- the ESP-IDF/device build never defines
// it, so bb_wifi_http_scan_wire_meta doesn't exist there.
//
// Fidelity finding vs the hand-authored s_scan_responses[0].schema literal
// (bb_wifi_http_routes.c): the "properties" content and the top-level
// "required" set (["aps"]) are BYTE-IDENTICAL, modulo two documented
// structural deltas inherent to the meta engine's fixed schema-shape policy
// (same class as test_bb_diag_storage_nvs_meta_golden.c's precedent):
//   1. no "required" list on the "aps" field's array-of-object "items"
//      schema -- bb_serialize_meta_openapi_schema()'s bb_oa_write_items()
//      only emits "type"/"properties"/"additionalProperties" for an
//      object-shaped array element, never "required". The hand literal's
//      ["ssid","rssi","secure"] on that items object is therefore stale --
//      corrected here in the golden's expected text rather than forced into
//      the meta table (the descriptor/engine is SSOT; the hand literal in
//      bb_wifi_http_routes.c itself is left as-is for this prework PR, which
//      only adds the meta table + golden -- fixing the shipped literal is
//      the mechanical PR-3 cutover step).
//   2. a trailing "additionalProperties":false at every object depth (both
//      the top-level object and the "aps" items object) -- the engine
//      always closes every rendered object; the hand literal predates that
//      policy and never sets it at either depth.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_wifi_http/bb_wifi_http_scan_wire_priv.h"

#include <string.h>

// Byte-fidelity target: s_scan_responses[0].schema's "properties"/top-level
// "required" content, re-expressed as bb_serialize_meta_openapi_schema()'s
// fixed object-schema shape (see file banner for the two documented deltas).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"aps\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"secure\":{\"type\":\"boolean\"}},"
    "\"additionalProperties\":false}}},"
    "\"required\":[\"aps\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting
// the table enough to render a schema from it.
void test_bb_wifi_http_scan_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_wifi_http_scan_wire_desc, &bb_wifi_http_scan_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces s_scan_responses[0].schema's
// field-level content exactly, modulo the two documented structural deltas
// above.
void test_bb_wifi_http_scan_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_wifi_http_scan_wire_desc, &bb_wifi_http_scan_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
