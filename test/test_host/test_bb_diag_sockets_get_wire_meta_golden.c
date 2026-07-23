// test_bb_diag_sockets_get_wire_meta_golden -- B1-1190 diag conversion:
// proves the #if-gated co-located bb_serialize_desc_meta_t pattern
// (B1-1059 PR-2a exemplar, test_bb_wifi_http_wire_meta_golden.c) for GET
// /api/diag/sockets (bb_diag_sockets_get_wire_desc /
// bb_diag_sockets_get_wire_meta, both in
// components/bb_diag_http/bb_diag_sockets_get_wire.c). Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see platformio.ini) --
// the ESP-IDF/device build never defines it, so bb_diag_sockets_get_wire_meta
// doesn't exist there.
//
// Fidelity finding vs the UPDATED hand-authored s_sockets_get_responses[]
// 200 literal (platform/espidf/bb_diag_http/bb_diag_http_routes.c,
// B1-1190): the descriptor is fully UNCONDITIONAL (no `.present` predicate
// anywhere -- every top-level field, all 11 by_state fields, and every
// per-PCB field are always emitted), so the host-derived meta schema
// carries the full shape with every field required, matching the hand
// literal's own updated by_state (11 named required integer fields,
// replacing the pre-B1-1190 `additionalProperties:{"type":"integer"}`) and
// top-level/pcbs content BYTE-IDENTICALLY. Two structural differences are
// inherent to the meta engine's fixed schema-shape policy, not this table's
// content, and are accepted as documented deltas (not a fidelity failure),
// same precedent as test_bb_ota_validator_partitions_wire_meta_golden.c and
// test_bb_diag_boot_wire_envelope_meta_golden.c:
//   1. a trailing "additionalProperties":false at every object depth (top
//      level, "by_state", and the "pcbs" items object) -- the engine always
//      closes every rendered object; the hand literal predates that policy
//      and never set it anywhere.
//   2. the "pcbs" ARR-of-OBJ items object never gets a "required" list --
//      bb_serialize_meta_openapi_schema()'s bb_oa_write_items() only emits
//      "type"/"properties"/"additionalProperties" for an object-shaped array
//      element, unlike bb_oa_write_field_schema()'s direct-BB_TYPE_OBJ
//      branch (which DOES emit "required" -- see "by_state" below, which IS
//      a direct BB_TYPE_OBJ field and DOES carry its "required" list). Every
//      "pcbs" row field IS marked `.required = true` in the co-located
//      s_diag_sockets_pcb_wire_meta_rows table; the composer simply has no
//      hook to render it at this nesting shape.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_diag_http/bb_diag_sockets_get_wire_priv.h"

#include <string.h>

// Byte-fidelity target: the UPDATED s_sockets_get_responses[]'s
// "properties"/"required" content, re-expressed as
// bb_serialize_meta_openapi_schema()'s fixed object-schema shape (see file
// banner for the two documented deltas).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"lwip_max_sockets\":{\"type\":\"integer\"},"
    "\"in_use\":{\"type\":\"integer\"},"
    "\"by_state\":{\"type\":\"object\",\"properties\":{"
    "\"CLOSED\":{\"type\":\"integer\"},"
    "\"LISTEN\":{\"type\":\"integer\"},"
    "\"SYN_SENT\":{\"type\":\"integer\"},"
    "\"SYN_RCVD\":{\"type\":\"integer\"},"
    "\"ESTABLISHED\":{\"type\":\"integer\"},"
    "\"FIN_WAIT_1\":{\"type\":\"integer\"},"
    "\"FIN_WAIT_2\":{\"type\":\"integer\"},"
    "\"CLOSE_WAIT\":{\"type\":\"integer\"},"
    "\"CLOSING\":{\"type\":\"integer\"},"
    "\"LAST_ACK\":{\"type\":\"integer\"},"
    "\"TIME_WAIT\":{\"type\":\"integer\"}},"
    "\"required\":[\"CLOSED\",\"LISTEN\",\"SYN_SENT\",\"SYN_RCVD\",\"ESTABLISHED\","
    "\"FIN_WAIT_1\",\"FIN_WAIT_2\",\"CLOSE_WAIT\",\"CLOSING\",\"LAST_ACK\",\"TIME_WAIT\"],"
    "\"additionalProperties\":false},"
    "\"pcbs\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"local_port\":{\"type\":\"integer\"},"
    "\"remote_ip\":{\"type\":\"string\"},"
    "\"remote_port\":{\"type\":\"integer\"},"
    "\"state\":{\"type\":\"string\"}},"
    "\"additionalProperties\":false}}},"
    "\"required\":[\"lwip_max_sockets\",\"in_use\",\"by_state\",\"pcbs\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans, recursing
// into both "by_state" and "pcbs"'s children) -- this is the same gate a
// future host generator would run before ever trusting the table enough to
// render a schema from it.
void test_bb_diag_sockets_get_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_diag_sockets_get_wire_desc,
                                    &bb_diag_sockets_get_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces the UPDATED s_sockets_get_responses[]
// field-level content exactly, modulo the two documented structural deltas
// above.
void test_bb_diag_sockets_get_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[2048];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_diag_sockets_get_wire_desc,
                                          &bb_diag_sockets_get_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
