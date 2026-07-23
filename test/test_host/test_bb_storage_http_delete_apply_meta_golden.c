// test_bb_storage_http_delete_apply_meta_golden -- B1-1181a: proves the
// #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a
// exemplar, test_bb_wifi_http_wire_meta_golden.c) for the DELETE
// /api/diag/storage REQUEST descriptor (s_storage_delete_desc /
// bb_storage_http_delete_apply_meta, both file-scope in
// platform/espidf/bb_diag_http/bb_storage_http_routes.c -- the desc is
// exposed here only via bb_storage_http_delete_apply_desc_for_test(), same
// posture as bb_storage_http_factory_reset_desc_for_test()). Name is
// deliberately distinct from the existing
// test_bb_storage_http_delete_wire_meta_golden.c (B1-1020), which covers a
// DIFFERENT descriptor entirely -- the 200 RESPONSE wire descriptor
// (bb_storage_http_delete_wire_desc, components/bb_diag_http/
// bb_storage_http_delete_wire.c). This file covers the REQUEST descriptor.
// Only reachable when BB_SERIALIZE_META_HOST is defined (this native env;
// see platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_storage_http_delete_apply_meta and the accessor don't exist there.
//
// The KEY finding this proves: "namespace" is bound TWICE on the wire
// (BB_TYPE_STR + BB_TYPE_ARR-of-STR, see s_storage_delete_fields' doc
// comment) and this genuinely IS a oneOf -- both occurrences are real,
// user-facing request shapes. The meta table carries ONE row for
// "namespace" (kind = BB_SERIALIZE_META_KIND_ONEOF, two branches pairing
// 1:1 with the STR-then-ARR physical occurrence order), rendering
// "namespace":{"oneOf":[{"type":"string"},{"type":"array","items":
// {"type":"string"}}]} -- never two separate "namespace" properties.
//
// Fidelity finding vs the hand-authored s_storage_delete_route's
// request_schema literal (bb_storage_http_routes.c): the per-field JSON
// Schema shape (including the "namespace" oneOf) and the "required" SET
// (same two keys: namespace, confirm) are byte-identical in substance.
// Three structural differences are inherent to the meta engine's fixed
// composition order/shape policy, not this table's content, and are
// accepted as documented deltas (not a fidelity failure):
//   1. "properties" key order follows s_storage_delete_fields' physical
//      field declaration order (confirm, wipe_wifi, backend, key,
//      namespace -- the composer walks desc->fields, not the meta table),
//      not the hand literal's authoring order (backend, namespace, key,
//      confirm, wipe_wifi).
//   2. the "required" array's ORDER likewise follows that same physical
//      field declaration order ("confirm" before "namespace"), not the
//      hand literal's arbitrary ["namespace","confirm"] authoring order --
//      same SET, different array order.
//   3. a trailing "additionalProperties":false -- the engine always closes
//      the outermost rendered object; the hand literal predates that
//      policy and never set it.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_storage_http.h"

#include <string.h>

// Byte-fidelity target: s_storage_delete_route's request_schema
// "properties"/"required" content, re-expressed as
// bb_serialize_meta_openapi_schema()'s fixed object-schema shape (see file
// banner for the one documented delta).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"confirm\":{\"type\":\"boolean\"},"
    "\"wipe_wifi\":{\"type\":\"boolean\"},"
    "\"backend\":{\"type\":\"string\"},"
    "\"key\":{\"type\":\"string\"},"
    "\"namespace\":{\"oneOf\":[{\"type\":\"string\"},"
    "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}]}},"
    "\"required\":[\"confirm\",\"namespace\"],"
    "\"additionalProperties\":false}";
// NOTE: "required" order is ["confirm","namespace"] -- physical field
// declaration order in s_storage_delete_fields, NOT the hand literal's
// ["namespace","confirm"] authoring order (see file banner, delta 1).

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, the "namespace" duplicate-key group
// correctly resolved as a 2-branch oneOf, no orphans) -- this is the same
// gate a future host generator would run before ever trusting the table
// enough to render a schema from it.
void test_bb_storage_http_delete_apply_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(bb_storage_http_delete_apply_desc_for_test(),
                                    &bb_storage_http_delete_apply_meta, err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces s_storage_delete_route's
// request_schema field-level content exactly, modulo the one documented
// structural delta above -- and, critically, "namespace" renders as a
// single combined oneOf property, never two separate "namespace" keys.
void test_bb_storage_http_delete_apply_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(bb_storage_http_delete_apply_desc_for_test(),
                                          &bb_storage_http_delete_apply_meta, buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
