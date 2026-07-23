// test_bb_wifi_http_diag_meta_golden -- B1-1180 PR-1: proves the #if-gated
// co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a exemplar,
// test_bb_wifi_http_wire_meta_golden.c) for the "wifi" bb_diag section
// (bb_wifi_http_diag_desc / bb_wifi_http_diag_meta, both in
// components/bb_wifi_http/bb_wifi_http_diag.c), proving byte-fidelity
// against bb_wifi_http_diag_schema -- the hand-authored, on-device (NOT
// host-gated) literal wired into the section's bb_diag_section_t.describe_route
// field (bb_wifi_http_diag_register(), same file) so bb_openapi_emit()
// describes GET /api/diag/wifi. Only reachable when BB_SERIALIZE_META_HOST
// is defined (this native env; see platformio.ini).
//
// One level of direct BB_TYPE_OBJ nesting ("reason_histogram") -- content is
// BYTE-IDENTICAL to bb_wifi_http_diag_schema; no engine-limitation delta
// applies. "reason_histogram"'s 13 count fields carry a `.present` predicate
// on the base bb_serialize_field_t (they're runtime-omitted when their
// bucket count is 0), but that's a RUNTIME emit concern the schema doesn't
// reflect (a JSON Schema describes the possible shape, not per-request
// omission) -- mirrors bb_diag_boot_wire.c's "boots_since"/"detail"/"age_s"
// precedent, which are likewise `.present`-gated fields with no meta-level
// distinction. Per this cluster's convention (also used by
// test_bb_diag_boot_wire_meta_golden.c), none of "reason_histogram"'s
// children are marked `.required` (the parent "reason_histogram" field
// itself IS required -- it's always present at the object level, unlike its
// individual bucket children).
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_wifi_http_diag.h"

#include <string.h>

void test_bb_wifi_http_diag_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_wifi_http_diag_desc, &bb_wifi_http_diag_meta,
                                    err, sizeof err));
}

void test_bb_wifi_http_diag_meta_golden_matches_hand_literal(void)
{
    char   buf[2048];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_wifi_http_diag_desc, &bb_wifi_http_diag_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_wifi_http_diag_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_wifi_http_diag_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
