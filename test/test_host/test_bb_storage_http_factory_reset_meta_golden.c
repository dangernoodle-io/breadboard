// test_bb_storage_http_factory_reset_meta_golden -- B1-1059 PR-2b-i-1:
// proves the #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059
// PR-2a exemplar, test_bb_wifi_http_wire_meta_golden.c) for the POST
// /api/diag/factory-reset REQUEST descriptor (s_factory_reset_desc /
// bb_storage_http_factory_reset_meta, both file-scope in
// platform/espidf/bb_diag_http/bb_storage_http_routes.c -- the desc is
// exposed here only via bb_storage_http_factory_reset_desc_for_test(),
// since this route has no companion _wire_priv.h the way the emit-side
// wire descriptors do). Only reachable when BB_SERIALIZE_META_HOST is
// defined (this native env; see platformio.ini) -- the ESP-IDF/device
// build never defines it, so bb_storage_http_factory_reset_meta and the
// accessor don't exist there.
//
// Fidelity finding vs the hand-authored s_factory_reset_route's
// request_schema literal (bb_storage_http_routes.c): the "properties"
// content and the "required" set are BYTE-IDENTICAL (single field
// "confirm", type string, required). One structural difference is inherent
// to the meta engine's fixed schema-shape policy, not this table's
// content, and is accepted as a documented delta (not a fidelity failure):
//   - a trailing top-level "additionalProperties":false -- the engine
//     always closes the outermost rendered object; the hand literal
//     predates that policy and never set it. (No top-level "title" delta
//     here -- the hand literal never carried one.)
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_storage_http.h"

#include <string.h>

// Byte-fidelity target: s_factory_reset_route's request_schema
// "properties"/"required" content, re-expressed as
// bb_serialize_meta_openapi_schema()'s fixed object-schema shape (see file
// banner for the one documented delta).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"confirm\":{\"type\":\"string\"}},"
    "\"required\":[\"confirm\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting the
// table enough to render a schema from it.
void test_bb_storage_http_factory_reset_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(bb_storage_http_factory_reset_desc_for_test(),
                                    &bb_storage_http_factory_reset_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces s_factory_reset_route's
// request_schema field-level content exactly, modulo the one documented
// structural delta above.
void test_bb_storage_http_factory_reset_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(bb_storage_http_factory_reset_desc_for_test(),
                                          &bb_storage_http_factory_reset_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
