// Host stub + test harness for the bb_health stack monitor.
// On host: bb_health_stack_monitor_init is a no-op (no FreeRTOS).
// Under BB_HEALTH_TESTING: exposes simulation helpers for unit tests.
#include "../../../components/bb_health/bb_health_stack.h"

#include <string.h>
#include <stdlib.h>

#ifdef BB_HEALTH_TESTING

// ---------------------------------------------------------------------------
// Test state
// ---------------------------------------------------------------------------

#define MAX_TEST_TASKS 32

typedef struct {
    char  name[64];
    bool  low;
} test_task_state_t;

static test_task_state_t s_test_states[MAX_TEST_TASKS];
static int               s_test_state_count = 0;
static int               s_post_count = 0;

// Find or insert test task state. Returns NULL if table full.
static test_task_state_t *find_or_insert(const char *name)
{
    for (int i = 0; i < s_test_state_count; i++) {
        if (strncmp(s_test_states[i].name, name,
                    sizeof(s_test_states[i].name)) == 0) {
            return &s_test_states[i];
        }
    }
    if (s_test_state_count >= MAX_TEST_TASKS) return NULL;
    test_task_state_t *e = &s_test_states[s_test_state_count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->low = false;
    return e;
}

bool bb_health_stack_simulate_post(const char *task_name, uint32_t free_bytes,
                                   uint32_t threshold, bool already_low)
{
    bool is_low = bb_health_stack_is_low(free_bytes, threshold);

    test_task_state_t *entry = find_or_insert(task_name);
    if (entry) {
        entry->low = already_low;
    }

    // Only post (increment counter) on transition into low.
    if (is_low && !already_low) {
        s_post_count++;
    }

    if (entry) {
        entry->low = is_low;
    }

    return is_low;
}

void bb_health_stack_reset_for_test(void)
{
    s_test_state_count = 0;
    s_post_count = 0;
    memset(s_test_states, 0, sizeof(s_test_states));
}

int bb_health_stack_post_count_for_test(void)
{
    return s_post_count;
}

#endif /* BB_HEALTH_TESTING */
