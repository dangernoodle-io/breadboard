#include "unity.h"
#include "bb_claim.h"

// Each test resets the claim via bb_claim_reset to ensure isolation.

// 1. Acquire on free claim → BB_OK, holder set.
void test_claim_acquire_free_returns_ok(void)
{
    bb_claim_t c = BB_CLAIM_INIT;
    bb_err_t rc = bb_claim_acquire(&c, "alice");
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("alice", bb_claim_holder(&c));
    bb_claim_reset(&c);
}

// 2. Acquire same id again → BB_OK (idempotent).
void test_claim_acquire_same_id_is_idempotent(void)
{
    bb_claim_t c = BB_CLAIM_INIT;
    TEST_ASSERT_EQUAL(BB_OK, bb_claim_acquire(&c, "alice"));
    TEST_ASSERT_EQUAL(BB_OK, bb_claim_acquire(&c, "alice"));
    TEST_ASSERT_EQUAL_STRING("alice", bb_claim_holder(&c));
    bb_claim_reset(&c);
}

// 3. Acquire different id → BB_ERR_CONFLICT.
void test_claim_acquire_different_id_returns_conflict(void)
{
    bb_claim_t c = BB_CLAIM_INIT;
    TEST_ASSERT_EQUAL(BB_OK, bb_claim_acquire(&c, "alice"));
    bb_err_t rc = bb_claim_acquire(&c, "bob");
    TEST_ASSERT_EQUAL(BB_ERR_CONFLICT, rc);
    // Original holder unchanged.
    TEST_ASSERT_EQUAL_STRING("alice", bb_claim_holder(&c));
    bb_claim_reset(&c);
}

// 4. Release by holder → holder becomes NULL.
void test_claim_release_by_holder_clears(void)
{
    bb_claim_t c = BB_CLAIM_INIT;
    bb_claim_acquire(&c, "alice");
    bb_claim_release(&c, "alice");
    TEST_ASSERT_NULL(bb_claim_holder(&c));
}

// 5. Release by non-holder → no-op, original holder unchanged.
void test_claim_release_by_non_holder_is_noop(void)
{
    bb_claim_t c = BB_CLAIM_INIT;
    bb_claim_acquire(&c, "alice");
    bb_claim_release(&c, "bob");
    TEST_ASSERT_EQUAL_STRING("alice", bb_claim_holder(&c));
    bb_claim_reset(&c);
}

// 6. bb_claim_holder on free claim returns NULL.
void test_claim_holder_free_returns_null(void)
{
    bb_claim_t c = BB_CLAIM_INIT;
    TEST_ASSERT_NULL(bb_claim_holder(&c));
}

// 7. bb_claim_reset clears the holder.
void test_claim_reset_clears_holder(void)
{
    bb_claim_t c = BB_CLAIM_INIT;
    bb_claim_acquire(&c, "alice");
    TEST_ASSERT_NOT_NULL(bb_claim_holder(&c));
    bb_claim_reset(&c);
    TEST_ASSERT_NULL(bb_claim_holder(&c));
}

// 8. After release, slot is free and another id can acquire.
void test_claim_release_then_reacquire(void)
{
    bb_claim_t c = BB_CLAIM_INIT;
    bb_claim_acquire(&c, "alice");
    bb_claim_release(&c, "alice");
    bb_err_t rc = bb_claim_acquire(&c, "bob");
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("bob", bb_claim_holder(&c));
    bb_claim_reset(&c);
}
