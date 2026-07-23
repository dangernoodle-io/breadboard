// test_bb_display_info_wire_meta_golden -- B1-1179: proves the #if-gated
// co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a exemplar,
// test_bb_wifi_http_wire_meta_golden.c) for the SSE "health.display" topic
// (bb_display_info_wire_desc / bb_display_info_wire_meta, both in
// components/display/bb_display/src/bb_display_info_wire.c) -- the flat,
// single-level descriptor this cluster was DROPPED from (B1-1059
// PR-2b-i) over a "panel" nullable-union issue. Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_display_info_wire_meta doesn't exist there.
//
// Fidelity finding vs the hand-authored k_display_schema literal
// (platform/espidf/bb_display/bb_display_info.c): the per-field
// "properties" content is BYTE-IDENTICAL (same field order, same field
// types) -- BUT ONLY AFTER a correction landed in this same PR. The
// literal used to mark "panel" a nullable `["string","null"]` union;
// bb_display_info_wire_desc's serializer never emits a JSON null for
// "panel" (or "width"/"height"/"enabled") -- all four carry a `.present`
// predicate (display_info_present(), bb_display_info_wire.c) that OMITS
// the key entirely from the rendered object when no display backend is
// registered (bb_serialize_walk.c's present-predicate skip), so "panel"
// is a plain optional string, corrected to match here. Beyond that
// correction, three structural differences are inherent to the meta
// engine's fixed schema-shape policy, not this table's content, and are
// accepted as documented deltas (not a fidelity failure) -- same class as
// bb_diag_boot_wire.c's exemplar:
//   1. no top-level "title" -- the composer has no descriptor-level title
//      hook; the hand literal's title is supplied via
//      bb_openapi_register_topic_schema()'s own title argument instead.
//   2. no top-level "x-sse-topic" -- an SSE-only annotation the composer
//      has no hook for at all; supplied by
//      bb_openapi_register_topic_schema()'s topic key argument instead.
//   3. "required":["present"] + a trailing "additionalProperties":false
//      -- the corrected hand literal now carries both explicitly (they
//      were absent from the pre-B1-1179 literal), matching the engine's
//      fixed policy of always closing the outermost rendered object this
//      way and always emitting a (possibly singleton) "required" array.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_display_info_wire.h"

#include <string.h>

// Byte-fidelity target: the corrected k_display_schema's
// "properties"/"required" content, re-expressed as
// bb_serialize_meta_openapi_schema()'s fixed object-schema shape (see
// file banner for the documented deltas).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"panel\":{\"type\":\"string\"},"
    "\"width\":{\"type\":\"integer\"},"
    "\"height\":{\"type\":\"integer\"},"
    "\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"present\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting
// the table enough to render a schema from it.
void test_bb_display_info_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_display_info_wire_desc, &bb_display_info_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces the corrected k_display_schema's
// field-level content exactly, modulo the documented structural deltas
// above.
void test_bb_display_info_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[512];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_display_info_wire_desc, &bb_display_info_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
