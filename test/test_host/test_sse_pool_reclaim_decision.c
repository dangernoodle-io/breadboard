#include "unity.h"
#include "sse_pool_reclaim_decision.h"

// B1-492: pure decision logic for the SSE task-bundle pool idle-reclaim tick,
// extracted from the ESP-IDF-only reclaim work_fn in
// bb_event_routes_espidf.c so Coveralls sees every branch of the
// keep-vs-destroy decision. Four cases (each of the three inputs can singly
// block destroy):
//   - active clients still connected -> keep
//   - a bundle is still acquired (mid-teardown, not yet released) -> keep
//   - a corpse hasn't reached eSuspended yet (still pending) -> keep
//   - all three zero -> destroy (idle, 0-byte standing cost)

void test_sse_pool_reclaim_decide_active_nonzero_keeps(void)
{
    TEST_ASSERT_EQUAL(SSE_POOL_RECLAIM_KEEP, sse_pool_reclaim_decide(1, 0, 0));
}

void test_sse_pool_reclaim_decide_acquired_nonzero_keeps(void)
{
    TEST_ASSERT_EQUAL(SSE_POOL_RECLAIM_KEEP, sse_pool_reclaim_decide(0, 1, 0));
}

void test_sse_pool_reclaim_decide_pending_nonzero_keeps(void)
{
    TEST_ASSERT_EQUAL(SSE_POOL_RECLAIM_KEEP, sse_pool_reclaim_decide(0, 0, 1));
}

void test_sse_pool_reclaim_decide_all_zero_destroys(void)
{
    TEST_ASSERT_EQUAL(SSE_POOL_RECLAIM_DESTROY, sse_pool_reclaim_decide(0, 0, 0));
}
