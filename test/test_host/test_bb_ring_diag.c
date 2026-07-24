// Tests for bb_ring_diag -- exercises bb_ring_diag_fill() (the exact
// production code path) against bb_queue_registry's host-backed foreach.
#include "unity.h"
#include "bb_ring_diag.h"
#include "bb_ring_diag_test.h"
#include "bb_queue.h"
#include "bb_queue_registry.h"

#include <stdbool.h>
#include <string.h>

static bb_queue_t make_ring(const char *name)
{
    bb_queue_t r = NULL;
    bb_err_t err = bb_queue_create(4, 8, BB_QUEUE_EVICT_OLDEST, name, &r);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(r);
    return r;
}

void test_bb_ring_diag_fill_null_dst_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_diag_fill(NULL, NULL));
}

void test_bb_ring_diag_fill_empty_registry(void)
{
    bb_queue_registry_test_reset();

    bb_ring_diag_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_diag_fill(&snap, NULL));

    TEST_ASSERT_EQUAL_INT64(0, snap.count);
    TEST_ASSERT_EQUAL_INT64(BB_QUEUE_REGISTRY_MAX, snap.registry_capacity);
    TEST_ASSERT_EQUAL_UINT(0, snap.rings.count);
    TEST_ASSERT_EQUAL_PTR(snap.rings_items, snap.rings.items);
}

void test_bb_ring_diag_fill_reports_live_rings(void)
{
    bb_queue_registry_test_reset();
    bb_queue_t a = make_ring("ring-a");
    bb_queue_t b = make_ring("ring-b");
    bb_queue_push(a, "x", 1, 0, 0);

    bb_ring_diag_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_diag_fill(&snap, NULL));

    TEST_ASSERT_EQUAL_INT64(2, snap.count);
    TEST_ASSERT_EQUAL_UINT(2, snap.rings.count);

    bool saw_a = false, saw_b = false;
    for (int64_t i = 0; i < snap.count; i++) {
        if (strcmp(snap.rings_items[i].name, "ring-a") == 0) {
            saw_a = true;
            TEST_ASSERT_EQUAL_INT64(1, snap.rings_items[i].count);
            TEST_ASSERT_EQUAL_INT64(4, snap.rings_items[i].capacity);
        } else if (strcmp(snap.rings_items[i].name, "ring-b") == 0) {
            saw_b = true;
            TEST_ASSERT_EQUAL_INT64(0, snap.rings_items[i].count);
        }
    }
    TEST_ASSERT_TRUE(saw_a);
    TEST_ASSERT_TRUE(saw_b);

    bb_queue_destroy(a);
    bb_queue_destroy(b);
    bb_queue_registry_test_reset();
}

// Registration fits the shared scratch buffer -- turns the "confirm the
// snapshot fits" requirement into an actual regression test.
void test_bb_ring_diag_desc_fits_scratch(void)
{
    bb_diag_section_test_reset();

    bb_diag_section_t section = {
        .name         = "rings",
        .desc         = "test",
        .snap_desc    = &bb_ring_diag_desc,
        .fill         = bb_ring_diag_fill,
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
 * const literal path, byte- and pointer-identical to bb_ring_diag_schema
 * (both assigned from the SAME BB_RING_DIAG_SCHEMA_LITERAL macro
 * invocation). B1-1059 PR-3 batch 1: config-OFF is a zero-diff no-op.
 * ---------------------------------------------------------------------------*/
void test_bb_ring_diag_describe_schema_is_unchanged_const_literal(void)
{
    // The for-test assemble is a documented no-op at this config gate (see
    // bb_ring_diag_test.h) -- still exercised here so the compiled-out
    // #else arm of bb_ring_diag_assemble_schema_for_test() is covered, not
    // just the accessor.
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_diag_assemble_schema_for_test());

    TEST_ASSERT_EQUAL_PTR(bb_ring_diag_schema,
                           bb_ring_diag_get_describe_schema_for_test());
}
