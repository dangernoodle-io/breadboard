// test_bb_ring_diag_meta_golden -- B1-1180 PR-1: proves the #if-gated
// co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a exemplar,
// test_bb_wifi_http_wire_meta_golden.c) for the "rings" bb_diag section
// (bb_ring_diag_desc / bb_ring_diag_meta, both in
// components/bb_ring_diag/bb_ring_diag.c), proving byte-fidelity against
// bb_ring_diag_schema -- the hand-authored, on-device (NOT host-gated)
// literal wired into the section's bb_diag_section_t.describe_route
// (bb_ring_diag_register(), same file) so bb_openapi_emit() describes GET
// /api/diag/rings. Only reachable when BB_SERIALIZE_META_HOST is defined
// (this native env; see platformio.ini).
//
// Fidelity finding: content is BYTE-IDENTICAL except ONE structural delta
// inherent to the meta engine's fixed schema-shape policy for a FIXED
// (non-stream) BB_TYPE_ARR-of-BB_TYPE_OBJ field, same class as the
// bb_ota_validator_partitions_wire.c precedent (B1-1059 PR-2b-i-1):
//   1. the "rings" field's nested "items" object never gets a "required"
//      list -- bb_serialize_meta_openapi_schema()'s bb_oa_write_items() only
//      emits "type"/"properties"/"additionalProperties" for an object-shaped
//      array element, unlike the direct-BB_TYPE_OBJ branch (which DOES emit
//      "required"). bb_ring_diag_schema was authored to already omit that
//      list on "rings"' items object, so there is no observable mismatch --
//      noted here only because every OTHER new-in-this-PR sibling golden
//      documents the same engine limitation for its own ARR-of-OBJ field.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_ring_diag.h"

#include <string.h>

void test_bb_ring_diag_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_ring_diag_desc, &bb_ring_diag_meta, err, sizeof err));
}

void test_bb_ring_diag_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_ring_diag_desc, &bb_ring_diag_meta, buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_ring_diag_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_ring_diag_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
