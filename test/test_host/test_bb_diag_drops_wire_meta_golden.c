// test_bb_diag_drops_wire_meta_golden -- B1-1444 PR3: proves the #if-gated
// co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a exemplar,
// test_bb_diag_heap_check_wire_meta_golden.c) for GET /api/diag/drops
// (bb_diag_drops_wire_desc / bb_diag_drops_wire_meta, both in
// components/bb_diag_http/bb_diag_drops_wire.c). Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see platformio.ini) --
// the ESP-IDF/device build never defines it, so bb_diag_drops_wire_meta
// doesn't exist there.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_diag_http/bb_diag_drops_wire_priv.h"

#include <string.h>

// Byte-fidelity target: s_drops_get_responses[]'s 200-response
// "properties"/"required" content, re-expressed as
// bb_serialize_meta_openapi_schema()'s fixed object-schema shape (see
// test_bb_diag_heap_check_wire_meta_golden.c's banner for the one
// documented structural delta -- a trailing "additionalProperties":false).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"writer_dropped\":{\"type\":\"integer\"},"
    "\"udp_dropped\":{\"type\":\"integer\"},"
    "\"event_dropped\":{\"type\":\"integer\"},"
    "\"flush_timeouts\":{\"type\":\"integer\"},"
    "\"render_fail\":{\"type\":\"integer\"},"
    "\"stale_discard\":{\"type\":\"integer\"}},"
    "\"required\":[\"writer_dropped\",\"udp_dropped\",\"event_dropped\","
    "\"flush_timeouts\",\"render_fail\",\"stale_discard\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans).
void test_bb_diag_drops_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_diag_drops_wire_desc,
                                    &bb_diag_drops_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table reproduces
// s_drops_get_responses[]'s field-level content exactly, modulo the one
// documented structural delta above.
void test_bb_diag_drops_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[512];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_diag_drops_wire_desc,
                                          &bb_diag_drops_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
