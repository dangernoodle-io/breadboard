// test_bb_diag_heap_check_wire_meta_golden -- B1-1054 diag conversion:
// proves the #if-gated co-located bb_serialize_desc_meta_t pattern
// (B1-1059 PR-2a exemplar, test_bb_wifi_http_wire_meta_golden.c) for GET
// /api/diag/heap-check (bb_diag_heap_check_wire_desc /
// bb_diag_heap_check_wire_meta, both in
// components/bb_diag_http/bb_diag_heap_check_wire.c). Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see platformio.ini) --
// the ESP-IDF/device build never defines it, so bb_diag_heap_check_wire_meta
// doesn't exist there.
//
// Fidelity finding vs the hand-authored s_heap_check_get_responses[] 200
// literal (platform/espidf/bb_diag_http/bb_diag_http_routes.c): the
// per-field "properties" content and the "required" set are BYTE-IDENTICAL
// (single field "integrity_ok", type boolean, required). One structural
// difference is inherent to the meta engine's fixed schema-shape policy, not
// this table's content, and is accepted as a documented delta (not a
// fidelity failure):
//   - a trailing top-level "additionalProperties":false -- the engine
//     always closes the outermost rendered object (same policy proven by
//     every other golden in test_bb_serialize_meta_openapi.c); the hand
//     literal predates that policy and never set it.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_diag_http/bb_diag_heap_check_wire_priv.h"

#include <string.h>

// Byte-fidelity target: s_heap_check_get_responses[]'s 200-response
// "properties"/"required" content, re-expressed as
// bb_serialize_meta_openapi_schema()'s fixed object-schema shape (see file
// banner for the one documented delta).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"integrity_ok\":{\"type\":\"boolean\"}},"
    "\"required\":[\"integrity_ok\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting the
// table enough to render a schema from it.
void test_bb_diag_heap_check_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_diag_heap_check_wire_desc,
                                    &bb_diag_heap_check_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces s_heap_check_get_responses[]'s
// field-level content exactly, modulo the one documented structural delta
// above.
void test_bb_diag_heap_check_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[512];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_diag_heap_check_wire_desc,
                                          &bb_diag_heap_check_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
