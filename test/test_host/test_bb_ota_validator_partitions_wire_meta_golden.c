// test_bb_ota_validator_partitions_wire_meta_golden -- B1-1059 PR-2b-i-1:
// proves the #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059
// PR-2a exemplar, test_bb_wifi_http_wire_meta_golden.c) for GET
// /api/update/partitions (bb_ota_validator_partitions_wire_desc /
// bb_ota_validator_partitions_wire_meta, both in
// components/bb_ota_validator/bb_ota_validator_partitions_wire.c). Only
// reachable when BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_ota_validator_partitions_wire_meta doesn't exist there.
//
// Fidelity finding vs the hand-authored s_partitions_responses[] literal
// (platform/espidf/bb_ota_validator/bb_ota_validator.c): the per-field
// "properties" content and every field's "type" are BYTE-IDENTICAL. THREE
// structural differences are inherent to the meta engine's fixed
// schema-shape policy, not this table's content, and are accepted as
// documented deltas (not a fidelity failure):
//   1. no top-level "title" -- same policy as the wifi exemplar (N/A here:
//      the hand literal never carried one either, so this is a non-issue
//      for THIS descriptor, just noted for completeness with the exemplar).
//   2. a trailing top-level "additionalProperties":false -- the engine
//      always closes the outermost rendered object; the hand literal
//      predates that policy and never set it.
//   3. the nested ARR-of-OBJ "items" object never gets a "required" list --
//      bb_serialize_meta_openapi_schema()'s bb_oa_write_items() only emits
//      "type"/"properties"/"additionalProperties" for an object-shaped array
//      element, unlike bb_oa_write_field_schema()'s direct-BB_TYPE_OBJ
//      branch (which DOES emit "required"). The hand literal's items object
//      lists all 5 fields as required; the composed schema omits that list
//      entirely for this shape. A genuine engine limitation, not a content
//      mismatch (every field IS marked .required = true in the co-located
//      meta table below -- the composer simply has no hook to render it at
//      this nesting shape).
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_ota_validator/bb_ota_validator_partitions_wire_priv.h"

#include <string.h>

// Byte-fidelity target: s_partitions_responses[]'s "properties"/"required"
// content, re-expressed as bb_serialize_meta_openapi_schema()'s fixed
// object-schema shape (see file banner for the three documented deltas).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"partitions\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"label\":{\"type\":\"string\"},"
    "\"address\":{\"type\":\"integer\"},"
    "\"size\":{\"type\":\"integer\"},"
    "\"running\":{\"type\":\"boolean\"},"
    "\"state\":{\"type\":\"string\"}},"
    "\"additionalProperties\":false}}},"
    "\"required\":[\"partitions\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans, recursing into
// the nested "partitions" row's children) -- this is the same gate a future
// host generator would run before ever trusting the table enough to render
// a schema from it.
void test_bb_ota_validator_partitions_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_ota_validator_partitions_wire_desc,
                                    &bb_ota_validator_partitions_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces s_partitions_responses[]'s
// field-level content exactly, modulo the three documented structural
// deltas above.
void test_bb_ota_validator_partitions_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_ota_validator_partitions_wire_desc,
                                          &bb_ota_validator_partitions_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
