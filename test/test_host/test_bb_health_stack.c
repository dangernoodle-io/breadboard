// Host unit tests for bb_health stack monitor:
// - bb_health_stack_is_low (pure decision)
// - bb_health_stack_build_json (pure JSON builder)
// - bb_health_stack_simulate_post (debounce / event-post logic via test hook)
#include "unity.h"
#include "../../components/bb_health/bb_health_stack.h"

#include <string.h>
#include <stdlib.h>

// setUp/tearDown called by Unity before/after each test.
// Full world reset happens in test_main.c's setUp() which also calls
// bb_health_stack_reset_for_test() via bb_health_reset_for_test().
// Call it here too for safety in the rare case tests run standalone.
static void local_reset(void)
{
    bb_health_stack_reset_for_test();
}

// ---------------------------------------------------------------------------
// bb_health_stack_is_low
// ---------------------------------------------------------------------------

void test_bb_health_stack_is_low_below_threshold(void)
{
    local_reset();
    TEST_ASSERT_TRUE(bb_health_stack_is_low(100, 512));
}

void test_bb_health_stack_is_low_at_threshold_is_not_low(void)
{
    local_reset();
    // Exactly at threshold is NOT low (strictly less than).
    TEST_ASSERT_FALSE(bb_health_stack_is_low(512, 512));
}

void test_bb_health_stack_is_low_above_threshold(void)
{
    local_reset();
    TEST_ASSERT_FALSE(bb_health_stack_is_low(1024, 512));
}

void test_bb_health_stack_is_low_zero_threshold(void)
{
    local_reset();
    // Nothing is below 0.
    TEST_ASSERT_FALSE(bb_health_stack_is_low(0, 0));
}

void test_bb_health_stack_is_low_zero_free_nonzero_threshold(void)
{
    local_reset();
    TEST_ASSERT_TRUE(bb_health_stack_is_low(0, 1));
}

// ---------------------------------------------------------------------------
// bb_health_stack_build_json
// ---------------------------------------------------------------------------

void test_bb_health_stack_build_json_low_true(void)
{
    local_reset();
    char buf[256];
    int n = bb_health_stack_build_json(buf, sizeof(buf), "main_task", 256, true);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"task\":\"main_task\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free_bytes\":256"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"low\":true"));
}

void test_bb_health_stack_build_json_low_false(void)
{
    local_reset();
    char buf[256];
    int n = bb_health_stack_build_json(buf, sizeof(buf), "idle0", 4096, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"task\":\"idle0\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free_bytes\":4096"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"low\":false"));
}

void test_bb_health_stack_build_json_null_buf_returns_neg1(void)
{
    local_reset();
    int n = bb_health_stack_build_json(NULL, 256, "task", 100, false);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_health_stack_build_json_zero_buf_sz_returns_neg1(void)
{
    local_reset();
    char buf[4];
    int n = bb_health_stack_build_json(buf, 0, "task", 100, false);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_health_stack_build_json_null_task_name_returns_neg1(void)
{
    local_reset();
    char buf[256];
    int n = bb_health_stack_build_json(buf, sizeof(buf), NULL, 100, false);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_health_stack_build_json_initial_snapshot(void)
{
    // The initial retained snapshot uses task="", free_bytes=0, low=false.
    local_reset();
    char buf[128];
    int n = bb_health_stack_build_json(buf, sizeof(buf), "", 0, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"task\":\"\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free_bytes\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"low\":false"));
}

// ---------------------------------------------------------------------------
// bb_health_stack_simulate_post (debounce logic)
// ---------------------------------------------------------------------------

void test_bb_health_stack_simulate_transition_into_low_posts(void)
{
    // First time task goes low (already_low=false, free < threshold) -> posts.
    local_reset();
    bool new_low = bb_health_stack_simulate_post("main_task", 100, 512, false);
    TEST_ASSERT_TRUE(new_low);
    TEST_ASSERT_EQUAL_INT(1, bb_health_stack_post_count_for_test());
}

void test_bb_health_stack_simulate_already_low_no_repost(void)
{
    // Task was already low last poll (already_low=true) -> no additional post.
    local_reset();
    bool new_low = bb_health_stack_simulate_post("main_task", 100, 512, true);
    TEST_ASSERT_TRUE(new_low);
    TEST_ASSERT_EQUAL_INT(0, bb_health_stack_post_count_for_test());
}

void test_bb_health_stack_simulate_normal_no_post(void)
{
    // Task free > threshold -> not low, no post.
    local_reset();
    bool new_low = bb_health_stack_simulate_post("main_task", 4096, 512, false);
    TEST_ASSERT_FALSE(new_low);
    TEST_ASSERT_EQUAL_INT(0, bb_health_stack_post_count_for_test());
}

void test_bb_health_stack_simulate_recovery_then_low_again_posts_once(void)
{
    // Transition into low -> posts.
    local_reset();
    bb_health_stack_simulate_post("task1", 100, 512, false);  // low, was ok -> post
    TEST_ASSERT_EQUAL_INT(1, bb_health_stack_post_count_for_test());

    // Recovery (already_low=true, now free > threshold) -> no post.
    bb_health_stack_simulate_post("task1", 1024, 512, true);  // ok, was low -> no post
    TEST_ASSERT_EQUAL_INT(1, bb_health_stack_post_count_for_test());

    // Goes low again (already_low=false) -> posts again.
    bb_health_stack_simulate_post("task1", 50, 512, false);
    TEST_ASSERT_EQUAL_INT(2, bb_health_stack_post_count_for_test());
}

void test_bb_health_stack_simulate_multiple_tasks(void)
{
    local_reset();
    // Two different tasks go low -> two posts.
    bb_health_stack_simulate_post("task_a", 100, 512, false);
    bb_health_stack_simulate_post("task_b", 200, 512, false);
    TEST_ASSERT_EQUAL_INT(2, bb_health_stack_post_count_for_test());
}

void test_bb_health_stack_simulate_at_threshold_not_low(void)
{
    // Exactly at threshold -> not low -> no post.
    local_reset();
    bb_health_stack_simulate_post("t", 512, 512, false);
    TEST_ASSERT_EQUAL_INT(0, bb_health_stack_post_count_for_test());
}

void test_bb_health_stack_reset_clears_state(void)
{
    local_reset();
    bb_health_stack_simulate_post("task", 100, 512, false);
    TEST_ASSERT_EQUAL_INT(1, bb_health_stack_post_count_for_test());

    bb_health_stack_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, bb_health_stack_post_count_for_test());
}
