// Host tests for floor's "possibly truncated" tasks-report warning guard
// (firmware review MEDIUM finding, B1-1418 PR-2). examples/floor is an
// example, not a component, so it is not part of the native scaffold's
// component graph and cannot be reached the normal way -- this test
// #includes floor_task_stack.c directly (a relative path into
// examples/floor/main/), the SAME translation unit floor_app.c links
// against, not a hand-mirrored copy. See floor_task_stack.h's doc for the
// full contract (including the verified pre-clamp-vs-clamped gather
// question) and floor_app.c's task_stack_log_tick for the live call site.
#include "unity.h"
#include "../../examples/floor/main/floor_task_stack.c"

// The exact-fit case this guard exists for: emitted == max_rows, never yet
// warned -- must fire.
void test_floor_task_stack_warns_when_emitted_equals_max_rows(void)
{
    TEST_ASSERT_TRUE(floor_task_stack_should_warn_truncated(24, 24, false));
}

// Comfortably under capacity: must never fire regardless of the latch.
void test_floor_task_stack_no_warn_when_emitted_below_max_rows(void)
{
    TEST_ASSERT_FALSE(floor_task_stack_should_warn_truncated(23, 24, false));
    TEST_ASSERT_FALSE(floor_task_stack_should_warn_truncated(0, 24, false));
}

// One-shot latch: already warned this boot must never fire again, even
// though the exact-fit condition still holds.
void test_floor_task_stack_already_warned_no_fire(void)
{
    TEST_ASSERT_FALSE(floor_task_stack_should_warn_truncated(24, 24, true));
}

// Degenerate zero-capacity case: emitted == max_rows == 0 must not warn --
// there are no rows to possibly truncate.
void test_floor_task_stack_zero_max_rows_no_fire(void)
{
    TEST_ASSERT_FALSE(floor_task_stack_should_warn_truncated(0, 0, false));
}
