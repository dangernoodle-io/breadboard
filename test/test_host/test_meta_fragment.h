#pragma once

// test_meta_fragment -- host-only golden-test helper for the "shape-(c)"
// exception class of B1-1059 co-located meta tables: /api/health SECTION
// fragments (bb_mqtt_client_health_section_desc, bb_temp_health_desc).
//
// Every other cluster's golden (REST: test_bb_ota_check_config_meta_golden.c
// etc.; SSE: test_bb_health_stack_wire_meta_golden.c etc.) can byte-compare
// bb_serialize_meta_openapi_schema()'s FULL output against the hand-authored
// literal, because both sides render a complete, top-level, standalone
// schema object (own "required" + own "additionalProperties":false).
//
// /api/health section fragments are different: bb_health_section_t.
// schema_props is spliced VERBATIM and BARE into the composite /api/health
// 200 schema -- no top-level "required", no top-level "additionalProperties"
// on the section's own object, because those decisions belong to the
// composite that inlines it, not the section. The composer
// (bb_serialize_meta_openapi_schema()) has no "bare fragment" mode -- it
// always closes every object with "required"+"additionalProperties":false.
// So a byte-equal assertion of the FULL composer output against the bare
// hand literal is structurally wrong for this cluster (it would always
// fail on the trailing keys, not because the field content disagrees).
//
// This helper narrows the comparison to what both sides truly share: the
// "properties":{...} object body. It string-locates the "properties" key,
// then walks forward with a string-aware brace-depth counter to find that
// value's own matching close brace (so a future nested OBJ/ARR-of-OBJ
// property, whose value itself contains braces, doesn't truncate early).
// Byte-comparing THAT extracted fragment against the hand literal's own
// "properties":{...} substring is the strongest fidelity check this
// shape supports without inventing a fragment-only render mode in the
// engine itself.
#include <stdbool.h>
#include <stddef.h>

// Extracts the `"properties":{...}` key/value substring (braces balanced,
// string-aware -- ignores braces inside quoted JSON strings) from `schema`
// into `out` (NUL-terminated). Returns false if `"properties":` isn't found,
// its value isn't an object, the braces never balance, or `out` is too
// small to hold the extracted span (including the NUL). Never writes a
// partial fragment on failure.
bool test_meta_fragment_extract_properties(const char *schema, char *out, size_t out_size);
