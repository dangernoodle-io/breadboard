// Tests for bb_diag_meminfo -- exercises bb_diag_meminfo_fill(), the thin
// bb_diag_fill_fn adapter over bb_meminfo_heap_snap_fill() (the exact
// production code path, no mirror). bb_meminfo_get() zero-fills on host, so
// the assertions here mirror test_snap_desc.c's own
// test_snap_desc_meminfo_snap_fill_host_zeroes coverage of the fill it
// delegates to.
#include "unity.h"
#include "bb_diag_meminfo.h"
#include "bb_meminfo_heap_snap.h"

#include <string.h>

void test_bb_diag_meminfo_fill_rejects_null_dst(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_meminfo_fill(NULL, NULL));
}

void test_bb_diag_meminfo_fill_host_zeroes_snapshot(void)
{
    bb_meminfo_heap_snap_t snap;
    memset(&snap, 0xAA, sizeof(snap));

    TEST_ASSERT_EQUAL(BB_OK, bb_diag_meminfo_fill(&snap, NULL));

    TEST_ASSERT_EQUAL_UINT64(0, snap.default_region.free);
    TEST_ASSERT_EQUAL_UINT64(0, snap.default_region.allocated);
    TEST_ASSERT_EQUAL_UINT64(0, snap.exec.free);
    TEST_ASSERT_EQUAL_UINT64(0, snap.exec.allocated);
    TEST_ASSERT_EQUAL_UINT64(0, snap.esp_min_free_heap);
}

// Registration fits the shared scratch buffer -- turns the "confirm the
// widened snap_desc still fits" requirement into an actual regression test
// (mirrors test_bb_diag_storage_nvs.c's own desc_fits_scratch test).
void test_bb_diag_meminfo_desc_fits_scratch(void)
{
    bb_diag_section_test_reset();

    bb_diag_section_t section = {
        .name         = "meminfo",
        .desc         = "test",
        .snap_desc    = &bb_meminfo_heap_snap_desc,
        .fill         = bb_diag_meminfo_fill,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&section));

    bb_diag_section_test_reset();
}
