// test_bb_diag_boot_wire_meta_golden -- B1-1059 PR-2b-i-2: proves the
// #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a
// exemplar, test_bb_wifi_http_wire_meta_golden.c) for the SSE "diag.boot"
// topic (bb_diag_boot_wire_desc / bb_diag_boot_wire_meta, both in
// components/bb_diag/bb_diag_boot_wire.c) -- the three-level-nested
// descriptor of this cluster (flat top-level fields, two nested
// BB_TYPE_OBJ fields "panic"/"reboot_reason", one nested
// BB_TYPE_ARR-of-BB_TYPE_OBJ field "reboot_history"). Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_diag_boot_wire_meta doesn't exist there.
//
// Fidelity finding vs the hand-authored k_diag_boot_schema literal
// (platform/espidf/bb_diag_http/bb_diag_http_routes.c): the per-field
// "properties" content at every nesting level is BYTE-IDENTICAL (same field
// order, same field types, same nesting shape), and the top-level
// "required" set is BYTE-IDENTICAL (all 7 top-level fields required in
// both). Four structural differences are inherent to the meta engine's
// fixed schema-shape policy and this descriptor's SSE-topic nature, not
// this table's content, and are accepted as documented deltas (not a
// fidelity failure):
//   1. no top-level "title" -- the composer has no descriptor-level title
//      hook (see the PR-2a exemplar's own delta #1); the hand literal's
//      "title":"DiagBoot" is supplied via
//      bb_openapi_register_topic_schema()'s own title argument instead.
//   2. no top-level "x-sse-topic" -- an SSE-only annotation the composer
//      has no hook for at all; supplied by
//      bb_openapi_register_topic_schema()'s topic key argument instead.
//   3. a trailing top-level "additionalProperties":false -- the engine
//      appends "additionalProperties":false to every rendered object (at
//      any nesting depth, including ARR-of-OBJ items and the top-level
//      object); the hand literal predates that policy and never set it.
//   4. every nested BB_TYPE_OBJ field ("panic", "reboot_reason") gets a
//      "required":[...]" (B1-1189: now populated, mirroring
//      bb_diag_boot_wire_desc's own `.present` gating -- see
//      bb_diag_boot_wire.c's meta-table banner; this is the derivation
//      target for the REST envelope's s_boot_get_responses[] literal, see
//      test_bb_diag_boot_wire_envelope_meta_golden.c) plus
//      "additionalProperties":false -- the engine always closes a
//      BB_TYPE_OBJ field's own schema this way regardless of content; the
//      hand k_diag_boot_schema literal never carries a "required" key on
//      either nested object AT ALL (same class of delta as the PR-2b-i-1
//      partitions-wire ARR-of-OBJ precedent, just for a direct-OBJ shape
//      instead of an ARR-of-OBJ shape -- the SSE topic schema simply omits
//      required-ness documentation at the nested level, regardless of what
//      the composer would emit there). "reboot_history"'s ARR-of-OBJ items
//      object carries neither "required" nor "additionalProperties":false
//      delta beyond what that precedent already documented -- the
//      composer's items branch DOES still close with
//      "additionalProperties":false (see below), matching the hand
//      literal's lack of a "required" key on its items object.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_diag_boot_wire.h"

#include <string.h>

// Byte-fidelity target: k_diag_boot_schema's "properties"/"required"
// content, re-expressed as bb_serialize_meta_openapi_schema()'s fixed
// object-schema shape (see file banner for the four documented deltas).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"wdt_resets\":{\"type\":\"integer\"},"
    "\"panic\":{\"type\":\"object\",\"properties\":{"
    "\"available\":{\"type\":\"boolean\"},"
    "\"boots_since\":{\"type\":\"integer\"}},"
    "\"required\":[\"available\"],\"additionalProperties\":false},"
    "\"pending_verify\":{\"type\":\"boolean\"},"
    "\"rolled_back\":{\"type\":\"boolean\"},"
    "\"reboot_reason\":{\"type\":\"object\",\"properties\":{"
    "\"source\":{\"type\":\"string\"},"
    "\"detail\":{\"type\":\"string\"},"
    "\"uptime_s\":{\"type\":\"integer\"},"
    "\"epoch_s\":{\"type\":\"integer\"},"
    "\"age_s\":{\"type\":\"integer\"}},"
    "\"required\":[\"source\",\"uptime_s\",\"epoch_s\"],\"additionalProperties\":false},"
    "\"reboot_history\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"source\":{\"type\":\"string\"},"
    "\"epoch_s\":{\"type\":\"integer\"},"
    "\"uptime_s\":{\"type\":\"integer\"}},"
    "\"additionalProperties\":false}}},"
    "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\","
    "\"pending_verify\",\"rolled_back\",\"reboot_reason\",\"reboot_history\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans, recursing
// into "panic"/"reboot_reason"/"reboot_history"'s children) -- this is the
// same gate a future host generator would run before ever trusting the
// table enough to render a schema from it.
void test_bb_diag_boot_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_diag_boot_wire_desc, &bb_diag_boot_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces k_diag_boot_schema's field-level
// content exactly, modulo the four documented structural deltas above.
void test_bb_diag_boot_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_diag_boot_wire_desc, &bb_diag_boot_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
