// Host tests for bb_registry — generic name→handle object registry.
//
// Coverage targets: register/deregister/freeze/foreach/count/get_by_index/
// lookup + error paths + HWM warn + compaction + reset.

#include "unity.h"
#include "bb_registry.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Test registry — capacity 4 for boundary tests
// ---------------------------------------------------------------------------

BB_REGISTRY_DEFINE_TAGGED(s_reg, 4, "test_bb_registry");

// Helpers — reset between tests
static void reset_reg(void)
{
    bb_registry_reset(&s_reg);
}

// Dummy values — addresses used as distinct void* handles
static int s_v1 = 1;
static int s_v2 = 2;
static int s_v3 = 3;
static int s_v4 = 4;
static int s_v5 = 5;

// ---------------------------------------------------------------------------
// register: null name/value
// ---------------------------------------------------------------------------

void test_bb_registry_register_null_name_returns_invalid_arg(void)
{
    reset_reg();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_registry_register(&s_reg, NULL, &s_v1));
}

void test_bb_registry_register_null_value_returns_invalid_arg(void)
{
    reset_reg();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_registry_register(&s_reg, "a", NULL));
}

// ---------------------------------------------------------------------------
// register: basic success
// ---------------------------------------------------------------------------

void test_bb_registry_register_basic(void)
{
    reset_reg();
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "alpha", &s_v1));
    TEST_ASSERT_EQUAL(1, bb_registry_count(&s_reg));
}

// ---------------------------------------------------------------------------
// register: duplicate name → BB_ERR_INVALID_STATE
// ---------------------------------------------------------------------------

void test_bb_registry_register_duplicate_returns_invalid_state(void)
{
    reset_reg();
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "dup", &s_v1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_registry_register(&s_reg, "dup", &s_v2));
    // Count should not have grown.
    TEST_ASSERT_EQUAL(1, bb_registry_count(&s_reg));
}

// ---------------------------------------------------------------------------
// register: HWM warn fires when count transitions to cap-1, then NO_SPACE at cap
// ---------------------------------------------------------------------------

void test_bb_registry_register_hwm_and_full(void)
{
    reset_reg();
    // capacity == 4; HWM warn fires AFTER count increments to cap-1 (3),
    // i.e. on the insert of "c" — one slot still free at that point.
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "a", &s_v1));
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "b", &s_v2));
    // Adding "c": count transitions 2→3 == cap-1; HWM warn fires after.
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "c", &s_v3));
    TEST_ASSERT_EQUAL(3, bb_registry_count(&s_reg));
    // hwm_warned now set; adding the 4th (last) entry still succeeds.
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "d", &s_v4));
    TEST_ASSERT_EQUAL(4, bb_registry_count(&s_reg));
    // 5th entry → BB_ERR_NO_SPACE
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                      bb_registry_register(&s_reg, "e", &s_v5));
    TEST_ASSERT_EQUAL(4, bb_registry_count(&s_reg));
}

// ---------------------------------------------------------------------------
// register after freeze → BB_ERR_INVALID_STATE
// ---------------------------------------------------------------------------

void test_bb_registry_register_after_freeze_returns_invalid_state(void)
{
    reset_reg();
    bb_registry_freeze(&s_reg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_registry_register(&s_reg, "z", &s_v1));
}

// ---------------------------------------------------------------------------
// freeze: idempotent
// ---------------------------------------------------------------------------

void test_bb_registry_freeze_idempotent(void)
{
    reset_reg();
    bb_registry_freeze(&s_reg);
    bb_registry_freeze(&s_reg); // second call must not crash
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_registry_register(&s_reg, "z", &s_v1));
}

// ---------------------------------------------------------------------------
// deregister: null name
// ---------------------------------------------------------------------------

void test_bb_registry_deregister_null_name_returns_invalid_arg(void)
{
    reset_reg();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_registry_deregister(&s_reg, NULL));
}

// deregister: not found
// ---------------------------------------------------------------------------

void test_bb_registry_deregister_not_found_returns_not_found(void)
{
    reset_reg();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_registry_deregister(&s_reg, "missing"));
}

// ---------------------------------------------------------------------------
// deregister on frozen registry → BB_ERR_INVALID_STATE
// ---------------------------------------------------------------------------

void test_bb_registry_deregister_frozen_returns_invalid_state(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "a", &s_v1);
    bb_registry_freeze(&s_reg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_registry_deregister(&s_reg, "a"));
}

// ---------------------------------------------------------------------------
// deregister: mid-table compaction preserves order
// ---------------------------------------------------------------------------

void test_bb_registry_deregister_mid_table_compaction(void)
{
    reset_reg();
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "first",  &s_v1));
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "middle", &s_v2));
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "last",   &s_v3));

    // Remove the middle entry.
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_deregister(&s_reg, "middle"));
    TEST_ASSERT_EQUAL(2, bb_registry_count(&s_reg));

    // Verify order via get_by_index.
    bb_registry_entry_t e;
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_get_by_index(&s_reg, 0, &e));
    TEST_ASSERT_EQUAL_STRING("first", e.name);
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_get_by_index(&s_reg, 1, &e));
    TEST_ASSERT_EQUAL_STRING("last", e.name);
}

// ---------------------------------------------------------------------------
// deregister: first entry
// ---------------------------------------------------------------------------

void test_bb_registry_deregister_first_entry(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "a", &s_v1);
    bb_registry_register(&s_reg, "b", &s_v2);
    bb_registry_register(&s_reg, "c", &s_v3);

    TEST_ASSERT_EQUAL(BB_OK, bb_registry_deregister(&s_reg, "a"));
    TEST_ASSERT_EQUAL(2, bb_registry_count(&s_reg));

    bb_registry_entry_t e;
    bb_registry_get_by_index(&s_reg, 0, &e);
    TEST_ASSERT_EQUAL_STRING("b", e.name);
}

// ---------------------------------------------------------------------------
// deregister: last entry
// ---------------------------------------------------------------------------

void test_bb_registry_deregister_last_entry(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "a", &s_v1);
    bb_registry_register(&s_reg, "b", &s_v2);
    bb_registry_register(&s_reg, "c", &s_v3);

    TEST_ASSERT_EQUAL(BB_OK, bb_registry_deregister(&s_reg, "c"));
    TEST_ASSERT_EQUAL(2, bb_registry_count(&s_reg));
}

// ---------------------------------------------------------------------------
// foreach: copy-out visits all entries
// ---------------------------------------------------------------------------

typedef struct {
    const char *names[8];
    void       *values[8];
    int         count;
} foreach_ctx_t;

static void foreach_collect(const char *name, void *value, void *ctx)
{
    foreach_ctx_t *c = (foreach_ctx_t *)ctx;
    if (c->count < 8) {
        c->names[c->count]  = name;
        c->values[c->count] = value;
        c->count++;
    }
}

void test_bb_registry_foreach_visits_all(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "x", &s_v1);
    bb_registry_register(&s_reg, "y", &s_v2);
    bb_registry_register(&s_reg, "z", &s_v3);

    foreach_ctx_t ctx = {0};
    bb_registry_foreach(&s_reg, foreach_collect, &ctx);

    TEST_ASSERT_EQUAL(3, ctx.count);
    // Order matches insertion order.
    TEST_ASSERT_EQUAL_STRING("x", ctx.names[0]);
    TEST_ASSERT_EQUAL_STRING("y", ctx.names[1]);
    TEST_ASSERT_EQUAL_STRING("z", ctx.names[2]);
    TEST_ASSERT_EQUAL_PTR(&s_v1, ctx.values[0]);
    TEST_ASSERT_EQUAL_PTR(&s_v2, ctx.values[1]);
    TEST_ASSERT_EQUAL_PTR(&s_v3, ctx.values[2]);
}

void test_bb_registry_foreach_null_cb_is_noop(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "a", &s_v1);
    // Must not crash.
    bb_registry_foreach(&s_reg, NULL, NULL);
}

void test_bb_registry_foreach_empty_registry(void)
{
    reset_reg();
    foreach_ctx_t ctx = {0};
    bb_registry_foreach(&s_reg, foreach_collect, &ctx);
    TEST_ASSERT_EQUAL(0, ctx.count);
}

// ---------------------------------------------------------------------------
// get_by_index: in-bounds and out-of-bounds
// ---------------------------------------------------------------------------

void test_bb_registry_get_by_index_in_bounds(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "first",  &s_v1);
    bb_registry_register(&s_reg, "second", &s_v2);

    bb_registry_entry_t e;
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_get_by_index(&s_reg, 0, &e));
    TEST_ASSERT_EQUAL_STRING("first", e.name);
    TEST_ASSERT_EQUAL_PTR(&s_v1, e.value);

    TEST_ASSERT_EQUAL(BB_OK, bb_registry_get_by_index(&s_reg, 1, &e));
    TEST_ASSERT_EQUAL_STRING("second", e.name);
}

void test_bb_registry_get_by_index_out_of_bounds_returns_not_found(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "a", &s_v1);

    bb_registry_entry_t e;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_registry_get_by_index(&s_reg, 1, &e));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_registry_get_by_index(&s_reg, 99, &e));
}

void test_bb_registry_get_by_index_null_out_returns_invalid_arg(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "a", &s_v1);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_registry_get_by_index(&s_reg, 0, NULL));
}

void test_bb_registry_get_by_index_empty_returns_not_found(void)
{
    reset_reg();
    bb_registry_entry_t e;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_registry_get_by_index(&s_reg, 0, &e));
}

// ---------------------------------------------------------------------------
// lookup: found and not-found
// ---------------------------------------------------------------------------

void test_bb_registry_lookup_found(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "key", &s_v3);
    TEST_ASSERT_EQUAL_PTR(&s_v3, bb_registry_lookup(&s_reg, "key"));
}

void test_bb_registry_lookup_not_found_returns_null(void)
{
    reset_reg();
    TEST_ASSERT_NULL(bb_registry_lookup(&s_reg, "missing"));
}

void test_bb_registry_lookup_miss_in_nonempty_registry(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "present", &s_v1);
    TEST_ASSERT_NULL(bb_registry_lookup(&s_reg, "absent"));
}

void test_bb_registry_lookup_null_name_returns_null(void)
{
    reset_reg();
    TEST_ASSERT_NULL(bb_registry_lookup(&s_reg, NULL));
}

// ---------------------------------------------------------------------------
// pointer-keyed variant — separate registry so name-keyed/ptr-keyed tests
// never mix on the same instance (per the header's documented invariant).
// ---------------------------------------------------------------------------

BB_REGISTRY_DEFINE_TAGGED(s_ptr_reg, 4, "test_bb_registry_ptr");

static void reset_ptr_reg(void)
{
    bb_registry_reset(&s_ptr_reg);
}

// Dummy keys — addresses used as distinct identity pointers.
static int s_k1, s_k2, s_k3, s_k4, s_k5;

void test_bb_registry_register_ptr_null_key_returns_invalid_arg(void)
{
    reset_ptr_reg();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_registry_register_ptr(&s_ptr_reg, NULL, &s_v1));
}

void test_bb_registry_register_ptr_null_value_returns_invalid_arg(void)
{
    reset_ptr_reg();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_registry_register_ptr(&s_ptr_reg, &s_k1, NULL));
}

void test_bb_registry_register_ptr_basic(void)
{
    reset_ptr_reg();
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1));
    TEST_ASSERT_EQUAL(1, bb_registry_count(&s_ptr_reg));
}

void test_bb_registry_register_ptr_duplicate_returns_invalid_state(void)
{
    reset_ptr_reg();
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v2));
    TEST_ASSERT_EQUAL(1, bb_registry_count(&s_ptr_reg));
}

void test_bb_registry_register_ptr_after_freeze_returns_invalid_state(void)
{
    reset_ptr_reg();
    bb_registry_freeze(&s_ptr_reg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1));
}

void test_bb_registry_register_ptr_hwm_and_full(void)
{
    reset_ptr_reg();
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1));
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register_ptr(&s_ptr_reg, &s_k2, &s_v2));
    // count transitions 2->3 == cap-1; HWM warn fires after.
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register_ptr(&s_ptr_reg, &s_k3, &s_v3));
    TEST_ASSERT_EQUAL(3, bb_registry_count(&s_ptr_reg));
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register_ptr(&s_ptr_reg, &s_k4, &s_v4));
    TEST_ASSERT_EQUAL(4, bb_registry_count(&s_ptr_reg));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                      bb_registry_register_ptr(&s_ptr_reg, &s_k5, &s_v5));
    TEST_ASSERT_EQUAL(4, bb_registry_count(&s_ptr_reg));
}

void test_bb_registry_deregister_ptr_null_key_returns_invalid_arg(void)
{
    reset_ptr_reg();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_registry_deregister_ptr(&s_ptr_reg, NULL));
}

void test_bb_registry_deregister_ptr_not_found_returns_not_found(void)
{
    reset_ptr_reg();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                      bb_registry_deregister_ptr(&s_ptr_reg, &s_k1));
}

void test_bb_registry_deregister_ptr_frozen_returns_invalid_state(void)
{
    reset_ptr_reg();
    bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1);
    bb_registry_freeze(&s_ptr_reg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_registry_deregister_ptr(&s_ptr_reg, &s_k1));
}

void test_bb_registry_deregister_ptr_mid_table_compaction(void)
{
    reset_ptr_reg();
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1));
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register_ptr(&s_ptr_reg, &s_k2, &s_v2));
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register_ptr(&s_ptr_reg, &s_k3, &s_v3));

    TEST_ASSERT_EQUAL(BB_OK, bb_registry_deregister_ptr(&s_ptr_reg, &s_k2));
    TEST_ASSERT_EQUAL(2, bb_registry_count(&s_ptr_reg));

    TEST_ASSERT_EQUAL_PTR(&s_v1, bb_registry_lookup_ptr(&s_ptr_reg, &s_k1));
    TEST_ASSERT_EQUAL_PTR(&s_v3, bb_registry_lookup_ptr(&s_ptr_reg, &s_k3));
    TEST_ASSERT_NULL(bb_registry_lookup_ptr(&s_ptr_reg, &s_k2));
}

void test_bb_registry_lookup_ptr_found(void)
{
    reset_ptr_reg();
    bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v3);
    TEST_ASSERT_EQUAL_PTR(&s_v3, bb_registry_lookup_ptr(&s_ptr_reg, &s_k1));
}

void test_bb_registry_lookup_ptr_not_found_returns_null(void)
{
    reset_ptr_reg();
    TEST_ASSERT_NULL(bb_registry_lookup_ptr(&s_ptr_reg, &s_k1));
}

void test_bb_registry_lookup_ptr_miss_in_nonempty_registry(void)
{
    reset_ptr_reg();
    bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1);
    TEST_ASSERT_NULL(bb_registry_lookup_ptr(&s_ptr_reg, &s_k2));
}

void test_bb_registry_lookup_ptr_null_key_returns_null(void)
{
    reset_ptr_reg();
    TEST_ASSERT_NULL(bb_registry_lookup_ptr(&s_ptr_reg, NULL));
}

void test_bb_registry_foreach_visits_ptr_keyed_entries(void)
{
    reset_ptr_reg();
    bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1);
    bb_registry_register_ptr(&s_ptr_reg, &s_k2, &s_v2);

    foreach_ctx_t ctx = {0};
    bb_registry_foreach(&s_ptr_reg, foreach_collect, &ctx);

    TEST_ASSERT_EQUAL(2, ctx.count);
    TEST_ASSERT_EQUAL_PTR(&s_k1, ctx.names[0]);
    TEST_ASSERT_EQUAL_PTR(&s_k2, ctx.names[1]);
    TEST_ASSERT_EQUAL_PTR(&s_v1, ctx.values[0]);
    TEST_ASSERT_EQUAL_PTR(&s_v2, ctx.values[1]);
}

// ---------------------------------------------------------------------------
// foreach_ptr — typed void* key wrapper for pointer-keyed instances
// ---------------------------------------------------------------------------

typedef struct {
    void *keys[8];
    void *values[8];
    int   count;
} foreach_ptr_ctx_t;

static void foreach_ptr_collect(void *key, void *value, void *ctx)
{
    foreach_ptr_ctx_t *c = (foreach_ptr_ctx_t *)ctx;
    if (c->count < 8) {
        c->keys[c->count]   = key;
        c->values[c->count] = value;
        c->count++;
    }
}

void test_bb_registry_foreach_ptr_visits_all(void)
{
    reset_ptr_reg();
    bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1);
    bb_registry_register_ptr(&s_ptr_reg, &s_k2, &s_v2);

    foreach_ptr_ctx_t ctx = {0};
    bb_registry_foreach_ptr(&s_ptr_reg, foreach_ptr_collect, &ctx);

    TEST_ASSERT_EQUAL(2, ctx.count);
    TEST_ASSERT_EQUAL_PTR(&s_k1, ctx.keys[0]);
    TEST_ASSERT_EQUAL_PTR(&s_k2, ctx.keys[1]);
    TEST_ASSERT_EQUAL_PTR(&s_v1, ctx.values[0]);
    TEST_ASSERT_EQUAL_PTR(&s_v2, ctx.values[1]);
}

void test_bb_registry_foreach_ptr_null_cb_is_noop(void)
{
    reset_ptr_reg();
    bb_registry_register_ptr(&s_ptr_reg, &s_k1, &s_v1);
    // Must not crash.
    bb_registry_foreach_ptr(&s_ptr_reg, NULL, NULL);
}

void test_bb_registry_foreach_ptr_empty_registry(void)
{
    reset_ptr_reg();
    foreach_ptr_ctx_t ctx = {0};
    bb_registry_foreach_ptr(&s_ptr_reg, foreach_ptr_collect, &ctx);
    TEST_ASSERT_EQUAL(0, ctx.count);
}

// ---------------------------------------------------------------------------
// reset: clears everything
// ---------------------------------------------------------------------------

void test_bb_registry_reset_clears_all(void)
{
    reset_reg();
    bb_registry_register(&s_reg, "a", &s_v1);
    bb_registry_freeze(&s_reg);
    TEST_ASSERT_EQUAL(1, bb_registry_count(&s_reg));

    bb_registry_reset(&s_reg);

    TEST_ASSERT_EQUAL(0, bb_registry_count(&s_reg));
    // After reset, register should succeed again.
    TEST_ASSERT_EQUAL(BB_OK, bb_registry_register(&s_reg, "b", &s_v2));
    TEST_ASSERT_EQUAL(1, bb_registry_count(&s_reg));
}
