// test_bb_meminfo_heap_snap_meta_golden -- B1-1180 PR-1: proves the
// #if-gated co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a
// exemplar, test_bb_wifi_http_wire_meta_golden.c) for the "meminfo" bb_diag
// section (bb_meminfo_heap_snap_desc / bb_meminfo_heap_snap_meta, both in
// components/bb_meminfo/src/bb_meminfo_heap_snap.c), proving byte-fidelity
// against bb_meminfo_heap_snap_schema -- the hand-authored, on-device (NOT
// host-gated) literal wired into the "meminfo" section's
// bb_diag_section_t.describe_route (bb_diag_meminfo_register(),
// components/bb_diag/bb_diag_meminfo.c) so bb_openapi_emit() describes GET
// /api/diag/meminfo. Only reachable when BB_SERIALIZE_META_HOST is defined
// (this native env; see platformio.ini).
//
// Five levels of direct BB_TYPE_OBJ nesting ("default"/"internal"/"dma"/
// "spiram"/"exec", all sharing the SAME region shape and the SAME
// co-located s_meminfo_region_heap_snap_meta_rows table) -- content is
// BYTE-IDENTICAL to bb_meminfo_heap_snap_schema; no engine-limitation delta
// applies here (unlike this cluster's ARR-of-OBJ siblings) because every
// nested object here is a DIRECT BB_TYPE_OBJ field, and
// bb_oa_write_field_schema()'s direct-OBJ branch DOES render a "required"
// list (unlike bb_oa_write_items()'s ARR-of-OBJ branch) -- so
// bb_meminfo_heap_snap_schema marks every region field required and the
// composer reproduces that exactly.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "bb_meminfo_heap_snap.h"

#include <string.h>

void test_bb_meminfo_heap_snap_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_meminfo_heap_snap_desc, &bb_meminfo_heap_snap_meta,
                                    err, sizeof err));
}

void test_bb_meminfo_heap_snap_meta_golden_matches_hand_literal(void)
{
    char   buf[4096];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_meminfo_heap_snap_desc, &bb_meminfo_heap_snap_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_meminfo_heap_snap_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_meminfo_heap_snap_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
