// Tests for bb_diag_meminfo -- exercises bb_diag_meminfo_fill(), the thin
// bb_diag_fill_fn adapter over bb_meminfo_heap_snap_fill() (the exact
// production code path, no mirror). bb_meminfo_get() zero-fills on host, so
// the assertions here mirror test_snap_desc.c's own
// test_snap_desc_meminfo_snap_fill_host_zeroes coverage of the fill it
// delegates to.
#include "unity.h"
#include "bb_diag_meminfo.h"
#include "bb_diag_meminfo_test.h"
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

/* ---------------------------------------------------------------------------
 * CONFIG_BB_OPENAPI_RUNTIME_META OFF (this env's default -- undefined) --
 * proves the describe route's 200-response schema is still the UNCHANGED
 * const literal path, content-identical to bb_meminfo_heap_snap_schema
 * (both assigned from the SAME BB_MEMINFO_HEAP_SNAP_SCHEMA_LITERAL macro
 * invocation -- but in TWO DIFFERENT translation units:
 * bb_meminfo_heap_snap.c owns the extern variable, bb_diag_meminfo.c's
 * `#else` arm owns the route's own static initializer, see that file's
 * comment on why it can't just reference the variable). GCC does not pool
 * identical string literals across TUs the way Apple clang's linker does,
 * so the two instances are separate objects at separate addresses -- content
 * equality, not pointer identity, is the toolchain-independent proof.
 * B1-1059 PR-3 batch 1: config-OFF is a zero-diff no-op.
 * ---------------------------------------------------------------------------*/
void test_bb_diag_meminfo_describe_schema_is_unchanged_const_literal(void)
{
    // The for-test assemble is a documented no-op at this config gate (see
    // bb_diag_meminfo_test.h) -- still exercised here so the compiled-out
    // #else arm of bb_diag_meminfo_assemble_schema_for_test() is covered,
    // not just the accessor.
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_meminfo_assemble_schema_for_test());

    const char *schema = bb_diag_meminfo_get_describe_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(bb_meminfo_heap_snap_schema, schema);
}
