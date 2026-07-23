// test_bb_temp_health_meta_golden -- B1-1059 PR-2b-i-3: proves the
// #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a
// exemplar, test_bb_wifi_http_wire_meta_golden.c) for the /api/health "temp"
// section (bb_temp_health_desc / bb_temp_health_meta). PLATFORM TWIN:
// platform/host/bb_temp/bb_temp.c and platform/espidf/bb_temp/bb_temp.c
// each carry their own byte-identical copy of both tables (mirrors how the
// rest of that twin pair stays in sync); this native test env links the
// host twin, so that's the copy exercised here. Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_temp_health_meta doesn't exist there.
//
// SHAPE-(C) EXCEPTION -- unlike every REST/SSE golden (byte-equal against
// the FULL composer output), this descriptor's hand-authored companion
// (k_temp_schema, in each platform twin) is a bare /api/health SECTION
// fragment: bb_health_section_t.schema_props is spliced VERBATIM into the
// /api/health composite's schema, so it has no top-level "required" and no
// top-level "additionalProperties" of its own -- those decisions belong to
// the composite that inlines it. bb_serialize_meta_openapi_schema() has no
// "bare fragment" render mode; it always renders a complete standalone
// object with both trailing keys. A byte-equal assert of its full output
// against the bare literal would therefore always fail on structure that
// has nothing to do with field-content fidelity.
//
// So this golden narrows the comparison to what both sides truly share:
// the "properties":{...} object body, extracted via
// test_meta_fragment_extract_properties() (test_meta_fragment.h) from both
// the composer's real output and the hand literal, then byte-compared.
// Finding: BYTE-IDENTICAL (same field order, same JSON Schema "type" per
// field: present boolean, soc_c number).
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"
#include "bb_temp.h"

#include "test_meta_fragment.h"

#include <string.h>

// k_temp_schema (platform/{host,espidf}/bb_temp/bb_temp.c) verbatim.
static const char *const k_hand_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"soc_c\":{\"type\":\"number\"}}}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting
// the table enough to render a schema from it. Unconditional (does not
// depend on the fragment-only weakening below).
void test_bb_temp_health_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_temp_health_desc, &bb_temp_health_meta, err, sizeof err));
}

// The golden itself: the "properties" fragment of
// bb_serialize_meta_openapi_schema()'s real output over the real production
// desc + meta table matches the "properties" fragment of the hand literal
// it will (eventually) replace, byte-for-byte.
void test_bb_temp_health_meta_golden_matches_hand_fragment(void)
{
    char   rendered[512];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_temp_health_desc, &bb_temp_health_meta,
                                          rendered, sizeof rendered, &n));

    char rendered_props[512];
    TEST_ASSERT_TRUE(test_meta_fragment_extract_properties(rendered, rendered_props,
                                                             sizeof rendered_props));

    char hand_props[512];
    TEST_ASSERT_TRUE(test_meta_fragment_extract_properties(k_hand_schema, hand_props,
                                                             sizeof hand_props));

    TEST_ASSERT_EQUAL_STRING(hand_props, rendered_props);
}

#endif /* BB_SERIALIZE_META_HOST */
