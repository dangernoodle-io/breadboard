// Host unit tests for bb_health stack low-water observer:
// - bb_health_stack_build_json (pure JSON builder)
//
// Task-registry unification PR3: the mark-and-sweep debounce table,
// is_low() decision, and simulate_post() test hook this file used to cover
// moved to bb_task_registry's base-scan job -- see
// test/test_host/test_bb_task_registry_base_scan.c for the low-stack
// transition + debounce coverage that replaces it.
#include "unity.h"
#include "../../components/bb_health/bb_health_stack.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// bb_health_stack_build_json
// ---------------------------------------------------------------------------

void test_bb_health_stack_build_json_low_true(void)
{
    char buf[256];
    int n = bb_health_stack_build_json(buf, sizeof(buf), "main_task", 256, true);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"task\":\"main_task\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free_bytes\":256"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"low\":true"));
}

void test_bb_health_stack_build_json_low_false(void)
{
    char buf[256];
    int n = bb_health_stack_build_json(buf, sizeof(buf), "idle0", 4096, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"task\":\"idle0\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free_bytes\":4096"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"low\":false"));
}

void test_bb_health_stack_build_json_null_buf_returns_neg1(void)
{
    int n = bb_health_stack_build_json(NULL, 256, "task", 100, false);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_health_stack_build_json_zero_buf_sz_returns_neg1(void)
{
    char buf[4];
    int n = bb_health_stack_build_json(buf, 0, "task", 100, false);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_health_stack_build_json_null_task_name_returns_neg1(void)
{
    char buf[256];
    int n = bb_health_stack_build_json(buf, sizeof(buf), NULL, 100, false);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_health_stack_build_json_initial_snapshot(void)
{
    // The initial retained snapshot uses task="", free_bytes=0, low=false.
    char buf[128];
    int n = bb_health_stack_build_json(buf, sizeof(buf), "", 0, false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"task\":\"\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free_bytes\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"low\":false"));
}
