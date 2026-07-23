// test_bb_diag_storage_nvs_meta_golden -- B1-1180 PR-1: proves the
// #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a
// exemplar, test_bb_wifi_http_wire_meta_golden.c) for the "storage/nvs"
// bb_diag section (bb_diag_storage_nvs_desc / bb_diag_storage_nvs_meta,
// both in components/bb_diag/bb_diag_storage_nvs.c), proving byte-fidelity
// against bb_diag_storage_nvs_schema -- the hand-authored, on-device (NOT
// host-gated) literal wired into the section's bb_diag_section_t.describe_route
// field (bb_diag_storage_nvs_register(), same file) so bb_openapi_emit()
// describes GET /api/diag/storage/nvs. Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see platformio.ini).
//
// Fidelity finding: content is BYTE-IDENTICAL except ONE structural delta
// inherent to the meta engine's fixed schema-shape policy for a
// BB_TYPE_ARR-of-BB_TYPE_OBJ field (BB_ARR_STREAM cardinality doesn't change
// the composer's items-schema rendering vs a FIXED array -- same class as
// the bb_ota_validator_partitions_wire.c precedent, B1-1059 PR-2b-i-1):
//   1. the "entries" field's nested "items" object never gets a "required"
//      list -- bb_serialize_meta_openapi_schema()'s bb_oa_write_items() only
//      emits "type"/"properties"/"additionalProperties" for an object-shaped
//      array element. bb_diag_storage_nvs_schema was authored to already
//      omit that list on "entries"' items object, so there is no observable
//      mismatch.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_diag_storage_nvs.h"

#include <string.h>

void test_bb_diag_storage_nvs_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_diag_storage_nvs_desc, &bb_diag_storage_nvs_meta,
                                    err, sizeof err));
}

void test_bb_diag_storage_nvs_meta_golden_matches_hand_literal(void)
{
    char   buf[1536];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_diag_storage_nvs_desc, &bb_diag_storage_nvs_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_diag_storage_nvs_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_diag_storage_nvs_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
