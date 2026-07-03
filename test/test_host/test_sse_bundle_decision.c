#include "unity.h"
#include "sse_bundle_decision.h"

// B1-484 finding 3: pure decision logic extracted from the ESP-IDF-only
// SSE task-bundle slot_reusable/slot_reap callbacks in
// bb_event_routes_espidf.c, so Coveralls sees every branch of the reuse
// decision even though the callbacks themselves (FreeRTOS-typed) cannot be
// host-compiled. Three cases, matching the callback's three real states:
//   - never-assigned handle -> issue immediately
//   - task still running (not yet eSuspended) -> not reusable yet, 503
//   - task suspended (exited, corpse) -> reap then reissue

void test_sse_bundle_decide_none_returns_issue(void)
{
    TEST_ASSERT_EQUAL(SSE_BUNDLE_ACTION_ISSUE, sse_bundle_decide(SSE_BUNDLE_TASK_NONE));
}

void test_sse_bundle_decide_running_returns_not_yet(void)
{
    TEST_ASSERT_EQUAL(SSE_BUNDLE_ACTION_NOT_YET, sse_bundle_decide(SSE_BUNDLE_TASK_RUNNING));
}

void test_sse_bundle_decide_suspended_returns_reap_then_issue(void)
{
    TEST_ASSERT_EQUAL(SSE_BUNDLE_ACTION_REAP_THEN_ISSUE, sse_bundle_decide(SSE_BUNDLE_TASK_SUSPENDED));
}
