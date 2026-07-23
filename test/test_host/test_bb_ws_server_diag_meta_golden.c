// test_bb_ws_server_diag_meta_golden -- B1-1180 PR-1: proves the #if-gated
// co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a exemplar,
// test_bb_wifi_http_wire_meta_golden.c) for the "websocket" bb_diag section
// (bb_ws_server_diag_desc / bb_ws_server_diag_meta, both in
// components/bb_ws_server/bb_ws_server_diag.c), proving byte-fidelity
// against bb_ws_server_diag_schema -- the hand-authored, on-device (NOT
// host-gated) literal wired into the section's bb_diag_section_t.describe_route
// field (bb_ws_server_diag_register(), same file) so bb_openapi_emit()
// describes GET /api/diag/websocket. Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see platformio.ini)
// -- the ESP-IDF/device build never defines it, so bb_ws_server_diag_meta
// doesn't exist there (bb_ws_server_diag_schema itself always exists, on
// both builds).
//
// Fidelity finding: content is BYTE-IDENTICAL -- this is the simplest of
// the six B1-1180 PR-1 sections (a single required integer field), so none
// of the composer's structural deltas documented by the other five goldens
// in this cluster (top-level "additionalProperties":false being the
// engine's own always-added policy) apply differently here; the top-level
// "additionalProperties":false is itself always emitted by the composer,
// which bb_ws_server_diag_schema was authored to match exactly (no delta).
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_ws_server_diag.h"

#include <string.h>

void test_bb_ws_server_diag_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_ws_server_diag_desc, &bb_ws_server_diag_meta,
                                    err, sizeof err));
}

void test_bb_ws_server_diag_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_ws_server_diag_desc, &bb_ws_server_diag_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_ws_server_diag_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_ws_server_diag_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
