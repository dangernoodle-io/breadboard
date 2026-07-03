#include "sse_bundle_decision.h"

sse_bundle_action_t sse_bundle_decide(sse_bundle_task_state_t state)
{
    if (state == SSE_BUNDLE_TASK_NONE) return SSE_BUNDLE_ACTION_ISSUE;
    if (state == SSE_BUNDLE_TASK_SUSPENDED) return SSE_BUNDLE_ACTION_REAP_THEN_ISSUE;
    return SSE_BUNDLE_ACTION_NOT_YET;  // SSE_BUNDLE_TASK_RUNNING
}
