// bb_task_registry — ESP-IDF shell for the periodic base-scan job
// (task-registry unification PR3). Thin: calls uxTaskGetSystemState, builds
// the portable row snapshot, and hands it to the pure evaluator
// (bb_task_registry_base_scan_apply). Replaces bb_health_stack's former
// self-scan (the PR1 stopgap) as the single source of truth for periodic
// FreeRTOS task-state polling.
//
// GATED on CONFIG_FREERTOS_USE_TRACE_FACILITY: uxTaskGetSystemState is only
// available when that Kconfig is set. If it is off, this file compiles to
// nothing but a no-op start fn (mirrors the retired bb_health_stack.c's own
// no-op path).
#include "../../../components/bb_task_registry/bb_task_registry_base_scan.h"

#include "bb_log.h"
#include "bb_mem.h"
#include "bb_timer.h"
#include "bb_str.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "bb_task_registry_base_scan";

// Kconfig bridge (see wiki Conventions#audit-class-regressions).
#ifdef CONFIG_BB_TASK_REGISTRY_BASE_POLL_MS
#define BB_TASK_REGISTRY_BASE_POLL_MS CONFIG_BB_TASK_REGISTRY_BASE_POLL_MS
#endif
#ifndef BB_TASK_REGISTRY_BASE_POLL_MS
#define BB_TASK_REGISTRY_BASE_POLL_MS 10000
#endif

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t s_scan_tick = 0;

// Slack added to uxTaskGetNumberOfTasks()'s count before allocating the
// uxTaskGetSystemState() snapshot buffers -- per FreeRTOS guidance, a task
// created between the two calls would otherwise be silently dropped from
// this scan (the 1-scan sweep grace window absorbs a single miss, but this
// is cheap enough to just avoid).
#define BB_TASK_REGISTRY_BASE_SCAN_SLACK 4

static void do_scan(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n == 0) {
        return;
    }
    n += BB_TASK_REGISTRY_BASE_SCAN_SLACK;

    TaskStatus_t *tasks = bb_malloc_prefer_spiram(n * sizeof(TaskStatus_t));
    if (!tasks) {
        bb_log_w(TAG, "malloc failed for task snapshot (n=%" PRIu32 ")", (uint32_t)n);
        return;
    }

    bb_task_registry_base_row_t *rows = bb_malloc_prefer_spiram(n * sizeof(bb_task_registry_base_row_t));
    if (!rows) {
        bb_log_w(TAG, "malloc failed for row snapshot (n=%" PRIu32 ")", (uint32_t)n);
        bb_mem_free(tasks);
        return;
    }

    UBaseType_t got = uxTaskGetSystemState(tasks, n, NULL);

    for (UBaseType_t i = 0; i < got; i++) {
        rows[i].handle = (void *)tasks[i].xHandle;
        bb_strlcpy(rows[i].name, tasks[i].pcTaskName, sizeof(rows[i].name));
        // uxTaskGetStackHighWaterMark returns words; convert to bytes.
        rows[i].free_bytes = (uint32_t)tasks[i].usStackHighWaterMark * sizeof(StackType_t);
    }

    bb_mem_free(tasks);

    uint32_t scan_tick = ++s_scan_tick;
    bb_task_registry_base_scan_apply(rows, (int)got, scan_tick);

    bb_mem_free(rows);
}

static void poll_work_fn(void *arg)
{
    (void)arg;
    do_scan();
}

static bb_periodic_timer_t s_timer = NULL;

bb_err_t bb_task_registry_base_scan_start(void)
{
    bb_err_t err = bb_timer_deferred_periodic_create(poll_work_fn, NULL, "bb_task_reg_scan", &s_timer);
    if (err != BB_OK) {
        return err;
    }
    return bb_timer_periodic_start(s_timer, (uint64_t)BB_TASK_REGISTRY_BASE_POLL_MS * 1000ULL);
}

#else /* CONFIG_FREERTOS_USE_TRACE_FACILITY not set */

bb_err_t bb_task_registry_base_scan_start(void)
{
    bb_log_w(TAG, "CONFIG_FREERTOS_USE_TRACE_FACILITY=n; base scan is a no-op");
    return BB_OK;
}

#endif /* CONFIG_FREERTOS_USE_TRACE_FACILITY */
