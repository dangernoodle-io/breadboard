// ESP-IDF stack high-water monitor for bb_health.
// Polls FreeRTOS task stack margins periodically via bb_timer, posts a
// retained "health.stack" bb_event topic on transitions into low-stack state.
//
// GATED on CONFIG_FREERTOS_USE_TRACE_FACILITY: uxTaskGetSystemState is only
// available when that Kconfig is set. If it is off, this file compiles to
// nothing but the bb_health_stack_monitor_init no-op stub below.

#include "../../../components/bb_health/bb_health_stack.h"

#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_log.h"
#include "bb_timer.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "bb_health_stack";

// ---------------------------------------------------------------------------
// Kconfig defaults (not set when compiled on host)
// ---------------------------------------------------------------------------

#ifndef CONFIG_BB_HEALTH_STACK_POLL_MS
#define CONFIG_BB_HEALTH_STACK_POLL_MS 10000
#endif

#ifndef CONFIG_BB_HEALTH_STACK_LOW_BYTES
#define CONFIG_BB_HEALTH_STACK_LOW_BYTES 256
#endif

#ifndef CONFIG_BB_HEALTH_STACK_MAX_TASKS
#define CONFIG_BB_HEALTH_STACK_MAX_TASKS 32
#endif

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static bb_event_topic_t s_topic = NULL;

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Debounce table: track which tasks were already in low state last poll
// so we only post on transition into low (not every poll).
#define MAX_TRACKED_TASKS CONFIG_BB_HEALTH_STACK_MAX_TASKS

typedef struct {
    char     name[configMAX_TASK_NAME_LEN + 1];
    bool     low;
} task_low_state_t;

static task_low_state_t s_low_states[MAX_TRACKED_TASKS];
static int              s_low_state_count = 0;

// Find or insert a task in the debounce table. Returns pointer to entry,
// or NULL if table is full (new task, no slot).
static task_low_state_t *find_or_insert(const char *name)
{
    for (int i = 0; i < s_low_state_count; i++) {
        if (strncmp(s_low_states[i].name, name,
                    sizeof(s_low_states[i].name)) == 0) {
            return &s_low_states[i];
        }
    }
    if (s_low_state_count >= MAX_TRACKED_TASKS) {
        return NULL;  // table full: can't track this task
    }
    task_low_state_t *e = &s_low_states[s_low_state_count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->low = false;
    return e;
}

static void check_tasks(void)
{
    if (!s_topic) return;

    // Snapshot all FreeRTOS tasks.
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n == 0) return;

    TaskStatus_t *tasks = malloc(n * sizeof(TaskStatus_t));
    if (!tasks) {
        bb_log_w(TAG, "malloc failed for task snapshot (n=%" PRIu32 ")",
                 (uint32_t)n);
        return;
    }

    UBaseType_t got = uxTaskGetSystemState(tasks, n, NULL);

    for (UBaseType_t i = 0; i < got; i++) {
        // uxTaskGetStackHighWaterMark returns words; convert to bytes.
        uint32_t free_bytes =
            (uint32_t)tasks[i].usStackHighWaterMark * sizeof(StackType_t);
        bool is_low = bb_health_stack_is_low(
            free_bytes, (uint32_t)CONFIG_BB_HEALTH_STACK_LOW_BYTES);

        task_low_state_t *entry = find_or_insert(tasks[i].pcTaskName);
        if (!entry) {
            // Table full — log once and skip.
            bb_log_w(TAG, "task table full; skipping %s", tasks[i].pcTaskName);
            continue;
        }

        // Only post on transition into low (not on every poll while low).
        if (is_low && !entry->low) {
            char payload[128];
            int n_written = bb_health_stack_build_json(
                payload, sizeof(payload),
                tasks[i].pcTaskName, free_bytes, true);
            if (n_written > 0) {
                size_t sz = (size_t)n_written < sizeof(payload)
                            ? (size_t)n_written : sizeof(payload) - 1;
                bb_event_post(s_topic, 1, payload, sz);
                bb_log_w(TAG, "task '%s' stack low: %" PRIu32 " bytes free",
                         tasks[i].pcTaskName, free_bytes);
            }
        }
        entry->low = is_low;
    }

    free(tasks);
}

static void poll_work_fn(void *arg)
{
    (void)arg;
    check_tasks();
}

static bb_periodic_timer_t s_timer = NULL;

static bb_err_t start_monitor(void)
{
    bb_err_t err = bb_timer_deferred_periodic_create(poll_work_fn, NULL, "bb_health_stack",
                                                     &s_timer);
    if (err != BB_OK) return err;
    return bb_timer_periodic_start(s_timer,
        (uint64_t)CONFIG_BB_HEALTH_STACK_POLL_MS * 1000ULL);
}

#else /* CONFIG_FREERTOS_USE_TRACE_FACILITY not set */

static bb_err_t start_monitor(void)
{
    bb_log_w(TAG, "CONFIG_FREERTOS_USE_TRACE_FACILITY=n; stack monitor is a no-op");
    return BB_OK;
}

#endif /* CONFIG_FREERTOS_USE_TRACE_FACILITY */

// ---------------------------------------------------------------------------
// Public init (called by bb_health_init after the HTTP server is up)
// ---------------------------------------------------------------------------

bb_err_t bb_health_stack_monitor_init(void)
{
    bb_err_t err = bb_event_topic_register(BB_HEALTH_STACK_TOPIC, &s_topic);
    if (err != BB_OK) {
        bb_log_w(TAG, "topic register failed: %d", (int)err);
        return err;
    }

#if defined(CONFIG_BB_HEALTH_AUTO_ATTACH) && CONFIG_BB_HEALTH_AUTO_ATTACH
    {
        bb_err_t attach_err = bb_event_routes_attach_ex(BB_HEALTH_STACK_TOPIC, true);
        if (attach_err != BB_OK) {
            bb_log_w(TAG, "auto-attach failed for '" BB_HEALTH_STACK_TOPIC "': %d",
                     (int)attach_err);
        }
    }
#endif

    // Publish initial retained snapshot so SSE clients connecting before the
    // first poll see a sane baseline state (no low task, low=false).
    {
        char payload[128];
        int n = bb_health_stack_build_json(payload, sizeof(payload), "", 0, false);
        if (n > 0) {
            size_t sz = (size_t)n < sizeof(payload)
                        ? (size_t)n : sizeof(payload) - 1;
            bb_event_post(s_topic, 0, payload, sz);
        }
    }

    return start_monitor();
}
