// test_bb_system_reboot_meta_golden -- B1-1181a: proves the #if-gated
// co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a exemplar,
// test_bb_wifi_http_wire_meta_golden.c) for the POST /api/reboot REQUEST
// descriptor (s_reboot_desc / bb_system_reboot_meta, both file-scope in
// platform/espidf/bb_system/bb_system_routes.c -- the desc is exposed here
// only via bb_system_reboot_desc_for_test(), same posture as
// bb_storage_http_factory_reset_desc_for_test()). Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see platformio.ini)
// -- the ESP-IDF/device build never defines it, so bb_system_reboot_meta
// and the accessor don't exist there.
//
// The KEY finding this proves: "ts" is bound TWICE on the wire (BB_TYPE_U64
// + an internal BB_TYPE_F64 divergence-guard shadow, see s_reboot_fields'
// doc comment) but this is NOT a oneOf -- the F64 occurrence never surfaces
// to any consumer. The meta table carries ONE row for "ts" (kind defaults
// to BB_SERIALIZE_META_KIND_FIELD, occurrence 0 -- the U64 occurrence) and
// NO row at all for the F64 occurrence; bb_serialize_meta_openapi_schema()
// renders "ts":{"type":"integer"} exactly once, never a
// "ts":{"oneOf":[...]}, and never "ts" twice.
//
// Fidelity finding vs the hand-authored s_reboot_route's request_schema
// literal (bb_system_routes.c): the "properties" content is BYTE-IDENTICAL
// (single-shaped "ts":{"type":"integer"}, "detail":{"type":"string"}).
// Two structural differences are inherent to the meta engine's fixed
// schema-shape policy, not this table's content, and are accepted as
// documented deltas (not a fidelity failure):
//   1. the hand literal has NO "required" key at all (neither field is
//      required); the engine always emits a (possibly empty)
//      "required":[] array.
//   2. a trailing "additionalProperties":false -- the engine always closes
//      the outermost rendered object; the hand literal predates that
//      policy and never set it.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_system.h"

#include <string.h>

// Byte-fidelity target: s_reboot_route's request_schema "properties"
// content, re-expressed as bb_serialize_meta_openapi_schema()'s fixed
// object-schema shape (see file banner for the two documented deltas).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"ts\":{\"type\":\"integer\"},"
    "\"detail\":{\"type\":\"string\"}},"
    "\"required\":[],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, the "ts" duplicate-key group correctly
// occurrence-tagged, no orphans) -- this is the same gate a future host
// generator would run before ever trusting the table enough to render a
// schema from it.
void test_bb_system_reboot_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(bb_system_reboot_desc_for_test(), &bb_system_reboot_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces s_reboot_route's request_schema
// field-level content exactly, modulo the two documented structural deltas
// above -- and, critically, "ts" is single-shaped ("type":"integer"), never
// a "oneOf" and never emitted twice.
void test_bb_system_reboot_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(bb_system_reboot_desc_for_test(), &bb_system_reboot_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
