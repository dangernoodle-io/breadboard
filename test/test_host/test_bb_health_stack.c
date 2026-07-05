// Host unit tests for bb_health stack monitor:
// - bb_health_stack_is_low (pure decision)
// - bb_health_stack_build_json (pure JSON builder)
// - bb_health_stack_simulate_post (debounce / event-post logic via test hook)
#include "unity.h"
#include "../../components/bb_health/bb_health_stack.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

// ---------------------------------------------------------------------------
// bb_health_stack_table_mark / bb_health_stack_table_sweep (B1-601
// mark-and-sweep, bounds the debounce table to live tasks)
// ---------------------------------------------------------------------------

#define TEST_TABLE_CAP 4

static void reset_table(bb_health_stack_entry_t *table, int cap)
{
    memset(table, 0, sizeof(*table) * (size_t)cap);
}

void test_bb_health_stack_table_mark_inserts_on_first_sight(void)
{
    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);

    bb_health_stack_entry_t *e = bb_health_stack_table_mark(table, TEST_TABLE_CAP, "task_a", 1);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->in_use);
    TEST_ASSERT_EQUAL_STRING("task_a", e->name);
    TEST_ASSERT_FALSE(e->low);
    TEST_ASSERT_EQUAL_UINT32(1, e->seen_tick);
}

void test_bb_health_stack_table_mark_refreshes_not_duplicates(void)
{
    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);

    bb_health_stack_entry_t *e1 = bb_health_stack_table_mark(table, TEST_TABLE_CAP, "task_a", 1);
    e1->low = true;  // simulate a debounce-state change between scans
    bb_health_stack_entry_t *e2 = bb_health_stack_table_mark(table, TEST_TABLE_CAP, "task_a", 2);

    TEST_ASSERT_EQUAL_PTR(e1, e2);  // same slot, no duplicate
    TEST_ASSERT_EQUAL_UINT32(2, e2->seen_tick);
    TEST_ASSERT_TRUE(e2->low);  // state preserved across re-sight

    int occupied = 0;
    for (int i = 0; i < TEST_TABLE_CAP; i++) {
        if (table[i].in_use) occupied++;
    }
    TEST_ASSERT_EQUAL_INT(1, occupied);
}

void test_bb_health_stack_table_sweep_frees_disappeared_task(void)
{
    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);

    bb_health_stack_table_mark(table, TEST_TABLE_CAP, "sse_1", 1);
    // "sse_1" not marked on scan 2 -> within grace (missed 1 scan), survives.
    int freed = bb_health_stack_table_sweep(table, TEST_TABLE_CAP, 2);
    TEST_ASSERT_EQUAL_INT(0, freed);
    TEST_ASSERT_TRUE(table[0].in_use);

    // Still not marked on scan 3 -> missed 2 consecutive scans, now swept.
    freed = bb_health_stack_table_sweep(table, TEST_TABLE_CAP, 3);
    TEST_ASSERT_EQUAL_INT(1, freed);
    int occupied = 0;
    for (int i = 0; i < TEST_TABLE_CAP; i++) {
        if (table[i].in_use) occupied++;
    }
    TEST_ASSERT_EQUAL_INT(0, occupied);
}

// ---------------------------------------------------------------------------
// Sweep grace window (B1-601 follow-up): a task transiently missed by a
// single uxTaskGetSystemState scan must not be swept -- and, critically, its
// `low` debounce state must survive untouched so a subsequent re-mark does
// not look like a fresh (never-recovered) transition into low.
// ---------------------------------------------------------------------------

void test_bb_health_stack_table_sweep_grace_preserves_low_across_single_miss(void)
{
    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);

    // Task marked low at tick 1.
    bb_health_stack_entry_t *e = bb_health_stack_table_mark(table, TEST_TABLE_CAP, "task_a", 1);
    e->low = true;

    // Scan 2: task is NOT re-marked (simulated transient miss). Sweep at
    // tick 2 must NOT free it (grace window), and `low` must stay untouched.
    int freed = bb_health_stack_table_sweep(table, TEST_TABLE_CAP, 2);
    TEST_ASSERT_EQUAL_INT(0, freed);
    TEST_ASSERT_TRUE(table[0].in_use);
    TEST_ASSERT_TRUE(table[0].low);

    // Task reappears and is marked again at tick 3 -- same slot, `low`
    // never dipped to false in between (no spurious false->true transition
    // would be observed by a caller polling `low` every scan).
    bb_health_stack_entry_t *e2 = bb_health_stack_table_mark(table, TEST_TABLE_CAP, "task_a", 3);
    TEST_ASSERT_EQUAL_PTR(e, e2);
    TEST_ASSERT_TRUE(e2->low);
}

void test_bb_health_stack_table_sweep_grace_boundary_frees_after_two_misses(void)
{
    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);

    bb_health_stack_table_mark(table, TEST_TABLE_CAP, "task_a", 1);

    // Missed scan 2 -- within grace, survives.
    int freed = bb_health_stack_table_sweep(table, TEST_TABLE_CAP, 2);
    TEST_ASSERT_EQUAL_INT(0, freed);

    // Missed scan 3 too -- two consecutive misses, grace exhausted, swept.
    freed = bb_health_stack_table_sweep(table, TEST_TABLE_CAP, 3);
    TEST_ASSERT_EQUAL_INT(1, freed);
    TEST_ASSERT_FALSE(table[0].in_use);
}

void test_bb_health_stack_table_sweep_keeps_live_task(void)
{
    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);

    bb_health_stack_table_mark(table, TEST_TABLE_CAP, "main_task", 1);
    bb_health_stack_table_mark(table, TEST_TABLE_CAP, "main_task", 2);  // still alive on scan 2
    int freed = bb_health_stack_table_sweep(table, TEST_TABLE_CAP, 2);

    TEST_ASSERT_EQUAL_INT(0, freed);
    TEST_ASSERT_TRUE(table[0].in_use);
}

void test_bb_health_stack_table_never_wedges_full_after_many_transient_tasks(void)
{
    // Simulate > cap distinct short-lived task names cycling through, one
    // new task appearing and one old task exiting each scan -- mirrors
    // sse_N pool churn. Before mark-and-sweep this insert-only table would
    // wedge full after CAP distinct names and start rejecting every new one.
    // With the sweep grace window, an exited task lingers one extra tick
    // before being freed, so steady-state occupancy is 2 (current +
    // previous), not 1 -- still bounded, never wedged.
    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);

    char name[16];
    bb_health_stack_entry_t *last = NULL;
    for (uint32_t tick = 1; tick <= (uint32_t)(TEST_TABLE_CAP * 5); tick++) {
        // Sweep first: whatever was marked on the previous tick and not
        // re-marked this tick is gone (the transient task exited) once
        // grace is exhausted.
        bb_health_stack_table_sweep(table, TEST_TABLE_CAP, tick);
        snprintf(name, sizeof(name), "sse_%u", (unsigned)tick);
        last = bb_health_stack_table_mark(table, TEST_TABLE_CAP, name, tick);
    }

    TEST_ASSERT_NOT_NULL(last);  // table never wedged "full"
    int occupied = 0;
    for (int i = 0; i < TEST_TABLE_CAP; i++) {
        if (table[i].in_use) occupied++;
    }
    TEST_ASSERT_EQUAL_INT(2, occupied);  // current + previous transient task
}

void test_bb_health_stack_table_mark_null_args(void)
{
    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);

    TEST_ASSERT_NULL(bb_health_stack_table_mark(NULL, TEST_TABLE_CAP, "x", 1));
    TEST_ASSERT_NULL(bb_health_stack_table_mark(table, 0, "x", 1));
    TEST_ASSERT_NULL(bb_health_stack_table_mark(table, TEST_TABLE_CAP, NULL, 1));
}

void test_bb_health_stack_table_sweep_null_args(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_health_stack_table_sweep(NULL, TEST_TABLE_CAP, 1));

    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);
    TEST_ASSERT_EQUAL_INT(0, bb_health_stack_table_sweep(table, 0, 1));
}

void test_bb_health_stack_table_mark_full_of_distinct_live_tasks_returns_null(void)
{
    bb_health_stack_entry_t table[TEST_TABLE_CAP];
    reset_table(table, TEST_TABLE_CAP);

    for (int i = 0; i < TEST_TABLE_CAP; i++) {
        char n[16];
        snprintf(n, sizeof(n), "task_%d", i);
        TEST_ASSERT_NOT_NULL(bb_health_stack_table_mark(table, TEST_TABLE_CAP, n, 1));
    }
    // All CAP slots are live (all marked on tick 1) -- a genuinely new name
    // with no sweep in between correctly reports "no room" rather than
    // silently evicting a live task.
    TEST_ASSERT_NULL(bb_health_stack_table_mark(table, TEST_TABLE_CAP, "one_too_many", 1));
}
