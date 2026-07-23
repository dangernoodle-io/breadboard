// test_bb_diag_panic_get_wire_meta_golden -- B1-1188 diag conversion:
// proves the #if-gated co-located bb_serialize_desc_meta_t pattern
// (B1-1059 PR-2a exemplar, test_bb_wifi_http_wire_meta_golden.c) for GET
// /api/diag/panic (bb_diag_panic_get_wire_desc / bb_diag_panic_get_wire_meta,
// both in components/bb_diag_http/bb_diag_panic_get_wire.c). Only reachable
// when BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_diag_panic_get_wire_meta doesn't exist there.
//
// Fidelity finding vs the hand-authored s_panic_get_responses[] 200 literal
// (platform/espidf/bb_diag_http/bb_diag_http_routes.c): the descriptor is
// UNCONDITIONAL (all 10 fields, locked present-predicate-only design -- see
// bb_diag_panic_get_wire_priv.h's banner), so the host-derived meta schema
// carries the full superset shape, matching the hand literal's own
// superset ("required":["available"] only) BYTE-IDENTICALLY. "backtrace" is
// an ARR of scalar BB_TYPE_I64 (not ARR-of-OBJ), so its "items" schema is
// simply {"type":"integer"} -- no nested-object "required"-list engine gap
// applies here (unlike the boot/reboot_history ARR-of-OBJ precedent). One
// structural difference is inherent to the meta engine's fixed schema-shape
// policy, not this table's content, and is accepted as a documented delta
// (not a fidelity failure):
//   - a trailing top-level "additionalProperties":false -- the engine
//     always closes the outermost rendered object (same policy proven by
//     every other golden in test_bb_serialize_meta_openapi.c); the hand
//     literal predates that policy and never set it.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_diag_http/bb_diag_panic_get_wire_priv.h"

#include <string.h>

// Byte-fidelity target: s_panic_get_responses[]'s 200-response
// "properties"/"required" content, re-expressed as
// bb_serialize_meta_openapi_schema()'s fixed object-schema shape (see file
// banner for the one documented delta).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"available\":{\"type\":\"boolean\"},"
    "\"boots_since\":{\"type\":\"integer\"},"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"log_tail\":{\"type\":\"string\"},"
    "\"task\":{\"type\":\"string\"},"
    "\"exc_pc\":{\"type\":\"integer\"},"
    "\"exc_cause\":{\"type\":\"integer\"},"
    "\"backtrace\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
    "\"panic_reason\":{\"type\":\"string\"},"
    "\"app_sha256\":{\"type\":\"string\"}},"
    "\"required\":[\"available\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans) -- this is
// the same gate a future host generator would run before ever trusting the
// table enough to render a schema from it.
void test_bb_diag_panic_get_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_diag_panic_get_wire_desc,
                                    &bb_diag_panic_get_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces s_panic_get_responses[]'s
// field-level content exactly, modulo the one documented structural delta
// above.
void test_bb_diag_panic_get_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_diag_panic_get_wire_desc,
                                          &bb_diag_panic_get_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
