// Host tests for bb_collection — humble ordered collection of caller-owned
// opaque items.
//
// Coverage targets: add + count, stable-sort ordering incl equal-order tie,
// table-full loud error (no corruption), foreach snapshot + sorted order,
// empty foreach no-op, NULL-arg guards.

#include "unity.h"
#include "bb_collection.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Test collection — capacity 4 for boundary tests
// ---------------------------------------------------------------------------

BB_COLLECTION_DEFINE(s_coll, 4);

static void reset_coll(void)
{
    bb_collection_reset(&s_coll);
}

static int s_v1 = 1;
static int s_v2 = 2;
static int s_v3 = 3;
static int s_v4 = 4;
static int s_v5 = 5;

// ---------------------------------------------------------------------------
// add: NULL-arg guards
// ---------------------------------------------------------------------------

void test_bb_collection_add_null_collection_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_collection_add(NULL, "a", &s_v1, 0));
}

void test_bb_collection_add_null_name_returns_invalid_arg(void)
{
    reset_coll();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_collection_add(&s_coll, NULL, &s_v1, 0));
}

// ---------------------------------------------------------------------------
// add + count: basic
// ---------------------------------------------------------------------------

void test_bb_collection_add_and_count_basic(void)
{
    reset_coll();
    TEST_ASSERT_EQUAL(0, bb_collection_count(&s_coll));
    TEST_ASSERT_EQUAL(BB_OK, bb_collection_add(&s_coll, "a", &s_v1, 0));
    TEST_ASSERT_EQUAL(1, bb_collection_count(&s_coll));
    TEST_ASSERT_EQUAL(BB_OK, bb_collection_add(&s_coll, "b", &s_v2, 0));
    TEST_ASSERT_EQUAL(2, bb_collection_count(&s_coll));
}

// add() accepts a NULL item — bb_collection never derefs it.
void test_bb_collection_add_null_item_accepted(void)
{
    reset_coll();
    TEST_ASSERT_EQUAL(BB_OK, bb_collection_add(&s_coll, "a", NULL, 0));
    TEST_ASSERT_EQUAL(1, bb_collection_count(&s_coll));
}

// ---------------------------------------------------------------------------
// count: NULL collection
// ---------------------------------------------------------------------------

void test_bb_collection_count_null_returns_zero(void)
{
    TEST_ASSERT_EQUAL(0, bb_collection_count(NULL));
}

// ---------------------------------------------------------------------------
// add: table-full loud error, no corruption
// ---------------------------------------------------------------------------

void test_bb_collection_add_full_returns_no_space_and_no_corruption(void)
{
    reset_coll();
    TEST_ASSERT_EQUAL(BB_OK, bb_collection_add(&s_coll, "a", &s_v1, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_collection_add(&s_coll, "b", &s_v2, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_collection_add(&s_coll, "c", &s_v3, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_collection_add(&s_coll, "d", &s_v4, 0));
    TEST_ASSERT_EQUAL(4, bb_collection_count(&s_coll));

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                      bb_collection_add(&s_coll, "e", &s_v5, 0));
    // Rejected add must not corrupt existing state.
    TEST_ASSERT_EQUAL(4, bb_collection_count(&s_coll));
}

// ---------------------------------------------------------------------------
// foreach: NULL-arg guards
// ---------------------------------------------------------------------------

static void unreachable_cb(const bb_collection_entry_t *entry, void *ctx)
{
    (void)entry;
    (void)ctx;
    TEST_FAIL_MESSAGE("callback should not be invoked");
}

void test_bb_collection_foreach_null_collection_is_noop(void)
{
    bb_collection_foreach(NULL, unreachable_cb, NULL);
}

void test_bb_collection_foreach_null_cb_is_noop(void)
{
    reset_coll();
    bb_collection_add(&s_coll, "a", &s_v1, 0);
    bb_collection_foreach(&s_coll, NULL, NULL);
}

// ---------------------------------------------------------------------------
// foreach: empty collection is a no-op
// ---------------------------------------------------------------------------

void test_bb_collection_foreach_empty_is_noop(void)
{
    reset_coll();
    bb_collection_foreach(&s_coll, unreachable_cb, NULL);
}

// ---------------------------------------------------------------------------
// foreach: snapshot + stable-sort by order, ties preserve insertion order
// ---------------------------------------------------------------------------

#define FOREACH_CTX_MAX 8

typedef struct {
    const char *names[FOREACH_CTX_MAX];
    const void *items[FOREACH_CTX_MAX];
    int         orders[FOREACH_CTX_MAX];
    size_t      count;
} foreach_ctx_t;

static void foreach_collect(const bb_collection_entry_t *entry, void *ctx)
{
    foreach_ctx_t *fc = (foreach_ctx_t *)ctx;
    fc->names[fc->count]  = entry->name;
    fc->items[fc->count]  = entry->item;
    fc->orders[fc->count] = entry->order;
    fc->count++;
}

void test_bb_collection_foreach_sorts_by_order(void)
{
    reset_coll();
    // Inserted out of order; foreach must yield ascending order.
    bb_collection_add(&s_coll, "third",  &s_v3, 30);
    bb_collection_add(&s_coll, "first",  &s_v1, 10);
    bb_collection_add(&s_coll, "second", &s_v2, 20);

    foreach_ctx_t ctx = {0};
    bb_collection_foreach(&s_coll, foreach_collect, &ctx);

    TEST_ASSERT_EQUAL(3, ctx.count);
    TEST_ASSERT_EQUAL_STRING("first",  ctx.names[0]);
    TEST_ASSERT_EQUAL_STRING("second", ctx.names[1]);
    TEST_ASSERT_EQUAL_STRING("third",  ctx.names[2]);
    TEST_ASSERT_EQUAL(10, ctx.orders[0]);
    TEST_ASSERT_EQUAL(20, ctx.orders[1]);
    TEST_ASSERT_EQUAL(30, ctx.orders[2]);
    TEST_ASSERT_EQUAL_PTR(&s_v1, ctx.items[0]);
    TEST_ASSERT_EQUAL_PTR(&s_v2, ctx.items[1]);
    TEST_ASSERT_EQUAL_PTR(&s_v3, ctx.items[2]);
}

void test_bb_collection_foreach_equal_order_preserves_insertion_order(void)
{
    reset_coll();
    bb_collection_add(&s_coll, "a", &s_v1, 5);
    bb_collection_add(&s_coll, "b", &s_v2, 5);
    bb_collection_add(&s_coll, "c", &s_v3, 5);

    foreach_ctx_t ctx = {0};
    bb_collection_foreach(&s_coll, foreach_collect, &ctx);

    TEST_ASSERT_EQUAL(3, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a", ctx.names[0]);
    TEST_ASSERT_EQUAL_STRING("b", ctx.names[1]);
    TEST_ASSERT_EQUAL_STRING("c", ctx.names[2]);
}

// Mixed: some equal orders alongside distinct orders, to exercise both the
// swap and no-swap paths of the insertion sort in one pass.
void test_bb_collection_foreach_mixed_order_and_ties(void)
{
    reset_coll();
    bb_collection_add(&s_coll, "b1", &s_v2, 10); // idx0
    bb_collection_add(&s_coll, "a",  &s_v1, 0);  // idx1, must move before b1
    bb_collection_add(&s_coll, "b2", &s_v3, 10); // idx2, tie with b1, keeps order
    bb_collection_add(&s_coll, "c",  &s_v4, 20); // idx3, already last

    foreach_ctx_t ctx = {0};
    bb_collection_foreach(&s_coll, foreach_collect, &ctx);

    TEST_ASSERT_EQUAL(4, ctx.count);
    TEST_ASSERT_EQUAL_STRING("a",  ctx.names[0]);
    TEST_ASSERT_EQUAL_STRING("b1", ctx.names[1]);
    TEST_ASSERT_EQUAL_STRING("b2", ctx.names[2]);
    TEST_ASSERT_EQUAL_STRING("c",  ctx.names[3]);
}

// bb_collection_add() from within a callback is disallowed by contract; this
// only verifies foreach's own snapshot is unaffected by state that changes
// after the snapshot is taken (self-contained, does not call add() from cb).
void test_bb_collection_foreach_reflects_snapshot_at_call_time(void)
{
    reset_coll();
    bb_collection_add(&s_coll, "a", &s_v1, 0);

    foreach_ctx_t ctx = {0};
    bb_collection_foreach(&s_coll, foreach_collect, &ctx);
    TEST_ASSERT_EQUAL(1, ctx.count);

    bb_collection_add(&s_coll, "b", &s_v2, 1);
    TEST_ASSERT_EQUAL(2, bb_collection_count(&s_coll));
}
