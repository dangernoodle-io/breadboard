// test_bb_diag_storage_partitions_meta_golden -- B1-1180 PR-1: proves the
// #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a
// exemplar, test_bb_wifi_http_wire_meta_golden.c) for the
// "storage/partitions" bb_diag section (bb_diag_storage_partitions_desc /
// bb_diag_storage_partitions_meta, both in
// components/bb_diag/bb_diag_storage_partitions.c), proving byte-fidelity
// against bb_diag_storage_partitions_schema -- the hand-authored, on-device
// (NOT host-gated) literal wired into the section's bb_diag_section_t.describe_route
// field (bb_diag_storage_partitions_register(), same file) so
// bb_openapi_emit() describes GET /api/diag/storage/partitions. Only
// reachable when BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini).
//
// Fidelity finding: content is BYTE-IDENTICAL except ONE structural delta
// inherent to the meta engine's fixed schema-shape policy for a FIXED
// BB_TYPE_ARR-of-BB_TYPE_OBJ field -- the SAME delta already documented by
// the upstream exemplar for this exact row shape,
// test_bb_ota_validator_partitions_wire_meta_golden.c (B1-1059 PR-2b-i-1):
//   1. the "rows" field's nested "items" object never gets a "required"
//      list -- bb_serialize_meta_openapi_schema()'s bb_oa_write_items() only
//      emits "type"/"properties"/"additionalProperties" for an object-shaped
//      array element. bb_diag_storage_partitions_schema was authored to
//      already omit that list on "rows"' items object, so there is no
//      observable mismatch.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_diag_storage_partitions.h"

#include <string.h>

void test_bb_diag_storage_partitions_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_diag_storage_partitions_desc, &bb_diag_storage_partitions_meta,
                                    err, sizeof err));
}

void test_bb_diag_storage_partitions_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_diag_storage_partitions_desc, &bb_diag_storage_partitions_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_diag_storage_partitions_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_diag_storage_partitions_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
