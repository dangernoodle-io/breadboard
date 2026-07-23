// test_bb_wifi_http_creds_wire_meta_golden -- B1-1178: proves the #if-gated
// co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a exemplar,
// test_bb_wifi_http_wire_meta_golden.c) for the PATCH /api/wifi REQUEST
// descriptor (bb_wifi_http_creds_wire_desc / bb_wifi_http_creds_wire_meta,
// both in components/bb_wifi_http/bb_wifi_http_creds_wire.c -- extracted
// out of platform/espidf/bb_wifi_http/bb_wifi_http_routes.c, which is
// ESP-IDF-only and could not host a meta table). Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see platformio.ini)
// -- the ESP-IDF/device build never defines it, so
// bb_wifi_http_creds_wire_meta doesn't exist there.
//
// Fidelity finding vs the hand-authored s_wifi_patch_route.request_schema
// literal (bb_wifi_http_routes.c): the "properties"/"required" content is
// BYTE-IDENTICAL modulo one documented delta:
//   1. a trailing "additionalProperties":false -- the engine always closes
//      every rendered object (same policy proven by every other golden in
//      test_bb_serialize_meta_openapi.c); the hand literal predates that
//      policy and never set it.
//
// B1-1186: "maxLength" (31 for ssid, 63 for password) now emits from the
// meta table's `.max_len` (bb_wifi_http_creds_wire.c), matching the hand
// literal exactly -- no longer a documented delta. `.max_len` there
// carries the true validation bound (BB_WIFI_PENDING_SSID_MAX/PASS_MAX),
// distinct from the base descriptor's oversized wire max_len (64/96, see
// bb_wifi_http_creds_wire_priv.h) which exists purely for reject-vs-
// truncate buffer sizing. Buffer sizing/truncation-safety remains enforced
// at the wire layer (BB_WIFI_HTTP_CREDS_WIRE_SSID_BUF/PASS_BUF) plus
// wifi_creds_apply()'s explicit bb_wifi_pending_validate_buf() re-check --
// this schema now documents the same bound for API consumers too.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_wifi_http/bb_wifi_http_creds_wire_priv.h"

#include <string.h>

// Byte-fidelity target: s_wifi_patch_route's request_schema
// "properties"/"required" content, re-expressed as
// bb_serialize_meta_openapi_schema()'s fixed object-schema shape (see file
// banner for the one remaining documented delta).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"ssid\":{\"type\":\"string\",\"maxLength\":31},"
    "\"password\":{\"type\":\"string\",\"maxLength\":63}},"
    "\"required\":[\"ssid\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting
// the table enough to render a schema from it.
void test_bb_wifi_http_creds_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_wifi_http_creds_wire_desc, &bb_wifi_http_creds_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces s_wifi_patch_route's
// request_schema field-level content exactly, modulo the two documented
// structural deltas above.
void test_bb_wifi_http_creds_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_wifi_http_creds_wire_desc, &bb_wifi_http_creds_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
