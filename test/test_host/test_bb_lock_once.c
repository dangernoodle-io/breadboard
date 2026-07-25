// Host tests for bb_lock_once_ensure() (B1-1203) -- the shared
// bb_lock_init()-guarded-by-bb_once_run_fallible() helper. Does NOT re-derive
// bb_once_run_fallible()'s own retry/latch contract (test_bb_once.c already
// proves that); this only covers bb_lock_once_ensure()'s own NULL-arg
// branches and its success path (the failure branch is not host-reproducible
// -- see bb_lock_once.c's own LCOV_EXCL comment).

#include "unity.h"
#include "bb_lock_once.h"

void test_bb_lock_once_ensure_null_once_returns_invalid_arg(void)
{
    bb_lock_t lock = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_once_ensure(NULL, NULL, &lock));
}

void test_bb_lock_once_ensure_null_lock_returns_invalid_arg(void)
{
    bb_once_t once = BB_ONCE_INIT;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_lock_once_ensure(&once, NULL, NULL));
}

void test_bb_lock_once_ensure_success_initializes_and_is_lockable(void)
{
    bb_once_t once = BB_ONCE_INIT;
    bb_lock_t lock = {0};
    bb_lock_config_t cfg = { .name = "once_ensure_test" };

    TEST_ASSERT_FALSE(bb_lock_is_initialized(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_once_ensure(&once, &cfg, &lock));
    TEST_ASSERT_TRUE(bb_lock_is_initialized(&lock));

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_lock(&lock));
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_unlock(&lock));

    bb_lock_destroy(&lock);
}

void test_bb_lock_once_ensure_second_call_is_noop_and_stays_ok(void)
{
    bb_once_t once = BB_ONCE_INIT;
    bb_lock_t lock = {0};

    TEST_ASSERT_EQUAL(BB_OK, bb_lock_once_ensure(&once, NULL, &lock));
    // Second call across the same *once observes DONE already latched --
    // must not re-run bb_lock_init() (which would leak/clobber the backend
    // handle) and must still report BB_OK.
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_once_ensure(&once, NULL, &lock));

    bb_lock_destroy(&lock);
}
