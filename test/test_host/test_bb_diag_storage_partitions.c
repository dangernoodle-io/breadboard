// Tests for bb_diag_storage_partitions -- exercises
// bb_diag_storage_partitions_fill() (the exact production code path)
// against the host bb_partition mock (platform/host/bb_partition/
// bb_partition.c, 5 fixed rows) via bb_partition_list().
#include "unity.h"
#include "bb_diag_storage_partitions.h"
#include "bb_diag_storage_partitions_test.h"
#include "bb_partition.h"
#include "bb_partition_serialize.h"

#include <string.h>

void test_bb_diag_storage_partitions_fill_widens_rows(void)
{
    bb_diag_storage_partitions_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_partitions_fill(&snap, NULL));

    // Host mock's first row is "nvs" (platform/host/bb_partition/bb_partition.c).
    TEST_ASSERT_EQUAL_STRING("nvs", snap.rows_items[0].label);
    TEST_ASSERT_EQUAL_STRING("data", snap.rows_items[0].type);
    TEST_ASSERT_EQUAL_STRING("nvs", snap.rows_items[0].subtype);
    TEST_ASSERT_EQUAL_UINT64(0x009000, snap.rows_items[0].offset);
    TEST_ASSERT_EQUAL_UINT64(0x006000, snap.rows_items[0].size);
    TEST_ASSERT_FALSE(snap.rows_items[0].running);
    TEST_ASSERT_FALSE(snap.rows_items[0].next_ota);
}

void test_bb_diag_storage_partitions_fill_running_and_next_ota_flags(void)
{
    bb_diag_storage_partitions_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_partitions_fill(&snap, NULL));

    // Host mock index 2 ("ota_0") is running; index 3 ("ota_1") is next_ota.
    TEST_ASSERT_TRUE(snap.rows_items[2].running);
    TEST_ASSERT_FALSE(snap.rows_items[2].next_ota);
    TEST_ASSERT_FALSE(snap.rows_items[3].running);
    TEST_ASSERT_TRUE(snap.rows_items[3].next_ota);
}

void test_bb_diag_storage_partitions_fill_row_count_and_carrier(void)
{
    bb_diag_storage_partitions_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_partitions_fill(&snap, NULL));

    // Host mock always reports exactly 5 partitions -- no truncation at the
    // default BB_DIAG_STORAGE_PARTITIONS_ROW_CAP (16).
    TEST_ASSERT_EQUAL_UINT64(5, snap.row_count);
    TEST_ASSERT_EQUAL_UINT(5, snap.rows.count);
    TEST_ASSERT_EQUAL_PTR(snap.rows_items, snap.rows.items);
}

void test_bb_diag_storage_partitions_fill_null_dst_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_storage_partitions_fill(NULL, NULL));
}

// bb_diag_storage_partitions.c hardcodes n_children against
// bb_partition_row_fields as a documented invariant (bb_partition_row_fields
// is an extern array with no visible bound cross-TU) -- this test guards
// that literal against drift by asserting it against the SAME runtime
// count bb_partition_serialize.c itself computes.
void test_bb_diag_storage_partitions_row_field_count_matches_partition_serialize(void)
{
    TEST_ASSERT_EQUAL_UINT16(7, bb_partition_row_n_fields);
}

// Registration fits the shared scratch buffer -- turns the "confirm both
// split snapshots fit" requirement into an actual regression test.
void test_bb_diag_storage_partitions_desc_fits_scratch(void)
{
    bb_diag_section_test_reset();

    bb_diag_section_t section = {
        .name         = "storage/partitions",
        .desc         = "test",
        .snap_desc    = &bb_diag_storage_partitions_desc,
        .fill         = bb_diag_storage_partitions_fill,
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
 * const literal path, byte- and pointer-identical to bb_diag_storage_
 * partitions_schema (both assigned from the SAME
 * BB_DIAG_STORAGE_PARTITIONS_SCHEMA_LITERAL macro invocation in
 * bb_diag_storage_partitions.c). B1-1059 PR-3 batch 1: config-OFF is a
 * zero-diff no-op.
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_partitions_describe_schema_is_unchanged_const_literal(void)
{
    // The for-test assemble is a documented no-op at this config gate (see
    // bb_diag_storage_partitions_test.h) -- still exercised here so the
    // compiled-out #else arm of bb_diag_storage_partitions_assemble_
    // schema_for_test() is covered, not just the accessor.
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_partitions_assemble_schema_for_test());

    TEST_ASSERT_EQUAL_PTR(bb_diag_storage_partitions_schema,
                           bb_diag_storage_partitions_get_describe_schema_for_test());
}
