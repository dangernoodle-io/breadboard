// test_bb_ota_hooks_wire_meta_golden -- B1-1059 PR-2b-i-2: proves the
// #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a
// exemplar, test_bb_wifi_http_wire_meta_golden.c) for the SSE "ota.progress"
// topic (bb_ota_hooks_wire_desc / bb_ota_hooks_wire_meta, both in
// components/bb_ota_hooks/bb_ota_hooks_wire.c). Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_ota_hooks_wire_meta doesn't exist there.
//
// Fidelity finding vs the hand-authored k_ota_progress_schema literal
// (platform/espidf/bb_ota_hooks/bb_ota_hooks.c): the "properties" content
// (including "state"'s enum) and the "required" set are BYTE-IDENTICAL (same
// field order, same field types, all three fields required). Three
// structural differences are inherent to the meta engine's fixed
// schema-shape policy and this descriptor's SSE-topic nature, not this
// table's content, and are accepted as documented deltas (not a fidelity
// failure):
//   1. no top-level "title" -- the composer has no descriptor-level title
//      hook (see the PR-2a exemplar's own delta #1); the hand literal's
//      "title":"OtaProgress" is supplied via
//      bb_openapi_register_topic_schema()'s own title argument instead.
//   2. no top-level "x-sse-topic" -- an SSE-only annotation the composer
//      has no hook for at all; supplied by
//      bb_openapi_register_topic_schema()'s topic key argument instead.
//   3. a trailing "additionalProperties":false -- the engine always closes
//      every rendered object; the hand literal predates that policy.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_ota_hooks_wire.h"

#include <string.h>

// Byte-fidelity target: k_ota_progress_schema's "properties"/"required"
// content, re-expressed as bb_serialize_meta_openapi_schema()'s fixed
// object-schema shape (see file banner for the three documented deltas).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"via\":{\"type\":\"string\"},"
    "\"state\":{\"type\":\"string\",\"enum\":[\"start\",\"progress\",\"success\",\"fail\",\"unknown\"]},"
    "\"pct\":{\"type\":\"integer\"}},"
    "\"required\":[\"via\",\"state\",\"pct\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting
// the table enough to render a schema from it.
void test_bb_ota_hooks_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_ota_hooks_wire_desc, &bb_ota_hooks_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces k_ota_progress_schema's field-level
// content exactly, modulo the three documented structural deltas above.
void test_bb_ota_hooks_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_ota_hooks_wire_desc, &bb_ota_hooks_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
