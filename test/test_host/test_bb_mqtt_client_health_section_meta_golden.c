// test_bb_mqtt_client_health_section_meta_golden -- B1-1059 PR-2b-i-3:
// proves the #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059
// PR-2a exemplar, test_bb_wifi_http_wire_meta_golden.c) for the /api/health
// "mqtt" section (bb_mqtt_client_health_section_desc /
// bb_mqtt_client_health_section_meta, both in
// components/bb_mqtt_client/src/bb_mqtt_client_health_section.c). Only
// reachable when BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_mqtt_client_health_section_meta doesn't exist there.
//
// SHAPE-(C) EXCEPTION -- unlike every REST/SSE golden (byte-equal against
// the FULL composer output), this descriptor's hand-authored companion
// (k_mqtt_schema, same .c file) is a bare /api/health SECTION fragment:
// bb_health_section_t.schema_props is spliced VERBATIM into the /api/health
// composite's schema, so it has no top-level "required" and no top-level
// "additionalProperties" of its own -- those decisions belong to the
// composite that inlines it. bb_serialize_meta_openapi_schema() has no
// "bare fragment" render mode; it always renders a complete standalone
// object with both trailing keys. A byte-equal assert of its full output
// against the bare literal would therefore always fail on structure that
// has nothing to do with field-content fidelity.
//
// So this golden calls bb_serialize_meta_openapi_fragment() directly --
// the section-fragment engine mode that renders exactly this bare shape
// ({"type":"object","properties":{...}}, no top-level "required"/
// "additionalProperties") -- and byte-compares its output against the hand
// literal. Finding: BYTE-IDENTICAL (same field order, same JSON Schema
// "type" per field: enabled/connected both boolean).
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"
#include "bb_mqtt_client.h"

#include <string.h>

// k_mqtt_schema (bb_mqtt_client_health_section.c) verbatim.
static const char *const k_hand_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"enabled\":{\"type\":\"boolean\"},"
    "\"connected\":{\"type\":\"boolean\"}}}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting
// the table enough to render a schema from it. Unconditional (does not
// depend on the fragment-only weakening below).
void test_bb_mqtt_client_health_section_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_mqtt_client_health_section_desc,
                                    &bb_mqtt_client_health_section_meta, err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_fragment()'s real output
// over the real production desc + meta table matches the hand literal it
// will (eventually) replace, byte-for-byte.
void test_bb_mqtt_client_health_section_meta_golden_matches_hand_fragment(void)
{
    char   rendered[512];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_fragment(&bb_mqtt_client_health_section_desc,
                                            &bb_mqtt_client_health_section_meta,
                                            rendered, sizeof rendered, &n));

    TEST_ASSERT_EQUAL_STRING(k_hand_schema, rendered);
}

#endif /* BB_SERIALIZE_META_HOST */
