// test_bb_diag_boot_wire_envelope_meta_golden -- B1-1189: proves the
// hand-authored REST response schema for GET /api/diag/boot
// (s_boot_get_responses[] in
// platform/espidf/bb_diag_http/bb_diag_http_routes.c) is provably derived
// from bb_diag_boot_wire_desc + bb_diag_boot_wire_meta (both
// components/bb_diag/bb_diag_boot_wire.c), unlike
// test_bb_diag_boot_wire_meta_golden.c's target (the SEPARATE "diag.boot"
// SSE topic schema, k_diag_boot_schema) -- this route's runtime render
// (bb_diag_boot_render_envelope(), components/bb_diag_http/
// bb_diag_http_boot_wire.c) wraps bb_data_render()'s "diag_boot" object
// payload in a fixed {"ts_ms":<int>,"data":<payload>} envelope (B1-1053
// PR1), so its OpenAPI response schema must wrap the composed inner schema
// in the SAME envelope shape. Only reachable when BB_SERIALIZE_META_HOST is
// defined (this native env; see platformio.ini) -- the ESP-IDF/device build
// never defines it, so bb_diag_boot_wire_meta doesn't exist there. This
// test does NOT touch the runtime gather/render path at all -- gather/emit
// stays byte-identical; only s_boot_get_responses[]'s schema-of-a-schema
// provenance is at stake here.
//
// Fidelity finding vs s_boot_get_responses[]: the envelope wrapper
// ("ts_ms"/"data" properties, "required":["ts_ms","data"]`) and every
// "data"-nested field's "type"/"properties"/"required" content (including
// both nested BB_TYPE_OBJ fields "panic"/"reboot_reason", now that
// bb_diag_boot_wire_meta's `.required` flags mirror the descriptor's own
// `.present` gating -- see bb_diag_boot_wire.c's meta-table banner) are
// BYTE-IDENTICAL to the hand literal. Two structural differences are
// inherent to the meta engine's fixed schema-shape policy, not this
// table's content, and are accepted as documented deltas (not a fidelity
// failure):
//   1. the "data" object gets a trailing "additionalProperties":false --
//      the engine always closes the outermost rendered object of any
//      bb_serialize_meta_openapi_schema() call; the hand literal's "data"
//      object predates that policy and never set it. (The outer envelope
//      wrapper itself is built directly by this test, not by the composer,
//      so it carries no such trailing key either, matching the hand
//      literal's own envelope-level omission exactly.)
//   2. "reboot_history"'s ARR-of-OBJ items object never gets a "required"
//      list at all -- same engine limitation documented by the
//      partitions-wire precedent (test_bb_ota_validator_partitions_wire_meta_golden.c)
//      and by test_bb_diag_boot_wire_meta_golden.c's own delta #4; every
//      field IS marked `.required = true` in the co-located
//      s_diag_reboot_hist_wire_meta_rows table, the composer simply has no
//      hook to render it at this nesting shape. The hand literal's items
//      object DOES list all 3 fields as required
//      ("required":["source","epoch_s","uptime_s"]) -- an accepted,
//      documented mismatch, not something this table can currently fix.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_diag_boot_wire.h"

#include <stdio.h>
#include <string.h>

// Byte-fidelity target: s_boot_get_responses[]'s 200-response schema
// literal, re-expressed as the fixed {"ts_ms","data"} envelope wrapping
// bb_serialize_meta_openapi_schema()'s composed "diag_boot" object schema
// (see file banner for the two documented deltas).
static const char *const k_expected_envelope_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"ts_ms\":{\"type\":\"integer\"},"
    "\"data\":{\"type\":\"object\",\"properties\":{"
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
    "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\",\"pending_verify\",\"rolled_back\","
    "\"reboot_reason\",\"reboot_history\"],"
    "\"additionalProperties\":false}},"
    "\"required\":[\"ts_ms\",\"data\"]}";

// The co-located table must first structurally agree with its paired
// descriptor -- same gate test_bb_diag_boot_wire_meta_golden.c already
// runs; re-run here so this file stands alone (and so a future edit that
// only touches this file still catches a desc/meta drift).
void test_bb_diag_boot_wire_envelope_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_diag_boot_wire_desc, &bb_diag_boot_wire_meta,
                                    err, sizeof err));
}

// The golden itself: wrap bb_serialize_meta_openapi_schema()'s composed
// "diag_boot" object schema (the real production desc + the real
// production co-located meta table, not test-local copies of either) in
// the fixed {"ts_ms","data"} envelope and confirm it reproduces
// s_boot_get_responses[]'s 200-response schema content exactly, modulo the
// two documented structural deltas above.
void test_bb_diag_boot_wire_envelope_meta_golden_matches_hand_literal(void)
{
    char   data_schema[1024];
    size_t data_len = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_diag_boot_wire_desc, &bb_diag_boot_wire_meta,
                                          data_schema, sizeof data_schema, &data_len));

    char   buf[1536];
    size_t n = 0;

    n += (size_t)snprintf(buf + n, sizeof(buf) - n,
        "{\"type\":\"object\",\"properties\":{"
        "\"ts_ms\":{\"type\":\"integer\"},"
        "\"data\":");
    TEST_ASSERT_TRUE(n + data_len < sizeof(buf));
    memcpy(buf + n, data_schema, data_len);
    n += data_len;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n,
        "},\"required\":[\"ts_ms\",\"data\"]}");

    TEST_ASSERT_EQUAL_STRING(k_expected_envelope_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_envelope_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
