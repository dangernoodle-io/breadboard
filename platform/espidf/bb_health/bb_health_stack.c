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
#include "bb_init.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "bb_openapi.h"
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

// Debounce table: track which tasks were already in low state last poll so
// we only post on transition into low (not every poll). Bounded by
// mark-and-sweep (B1-601): every poll cycle tags each live task's entry with
// the current scan tick (mark), then frees entries whose tick doesn't match
// (sweep) -- their tasks have exited. Without this, short-lived pool tasks
// that cycle through names (e.g. sse_N) would accumulate a permanent slot
// each, wedging the table full for the rest of the boot. Table type and the
// mark/sweep functions themselves are pure (bb_health_stack.h) and
// host-tested in test/test_host/test_bb_health_stack.c.
#define MAX_TRACKED_TASKS CONFIG_BB_HEALTH_STACK_MAX_TASKS

static bb_health_stack_entry_t s_low_states[MAX_TRACKED_TASKS];
static uint32_t                s_scan_tick = 0;

static void check_tasks(void)
{
    if (!s_topic) return;

    // Snapshot all FreeRTOS tasks.
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n == 0) return;

    TaskStatus_t *tasks = bb_malloc_prefer_spiram(n * sizeof(TaskStatus_t));
    if (!tasks) {
        bb_log_w(TAG, "malloc failed for task snapshot (n=%" PRIu32 ")",
                 (uint32_t)n);
        return;
    }

    UBaseType_t got = uxTaskGetSystemState(tasks, n, NULL);

    uint32_t scan_tick = ++s_scan_tick;

    for (UBaseType_t i = 0; i < got; i++) {
        // uxTaskGetStackHighWaterMark returns words; convert to bytes.
        uint32_t free_bytes =
            (uint32_t)tasks[i].usStackHighWaterMark * sizeof(StackType_t);
        bool is_low = bb_health_stack_is_low(
            free_bytes, (uint32_t)CONFIG_BB_HEALTH_STACK_LOW_BYTES);

        bb_health_stack_entry_t *entry = bb_health_stack_table_mark(
            s_low_states, MAX_TRACKED_TASKS, tasks[i].pcTaskName, scan_tick);
        if (!entry) {
            // Table full of distinct live tasks — logs every occurrence and skips
            // (no rate-limiting).
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

    bb_mem_free(tasks);

    // Sweep entries not seen this scan — their tasks have exited.
    bb_health_stack_table_sweep(s_low_states, MAX_TRACKED_TASKS, scan_tick);
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
// PRE_HTTP lifecycle entry point
// ---------------------------------------------------------------------------

// Starts the periodic stack-monitor timer. No topic/openapi/event side
// effects — those stay in bb_health_stack_monitor_init (regular tier, called
// from bb_health_init). No-op (returns BB_OK) when
// CONFIG_FREERTOS_USE_TRACE_FACILITY=n (see the stub start_monitor above).
bb_err_t bb_health_stack_monitor_start(void)
{
    return start_monitor();
}

#if CONFIG_BB_HEALTH_STACK_AUTOSTART
BB_INIT_REGISTER_PRE_HTTP(bb_health_stack, bb_health_stack_monitor_start);
#endif

// ---------------------------------------------------------------------------
// Public init (called by bb_health_init after the HTTP server is up)
// ---------------------------------------------------------------------------

static const char k_health_stack_schema[] =
    "{\"title\":\"HealthStack\",\"x-sse-topic\":\"health.stack\",\"type\":\"object\","
    "\"properties\":{"
    "\"task\":{\"type\":\"string\"},"
    "\"free_bytes\":{\"type\":\"integer\"},"
    "\"low\":{\"type\":\"boolean\"}},"
    "\"required\":[\"task\",\"free_bytes\",\"low\"]}";

bb_err_t bb_health_stack_monitor_init(void)
{
    bb_err_t err = bb_event_topic_register(BB_HEALTH_STACK_TOPIC, &s_topic);
    if (err != BB_OK) {
        bb_log_w(TAG, "topic register failed: %d", (int)err);
        return err;
    }

    bb_openapi_register_topic_schema(BB_HEALTH_STACK_TOPIC, k_health_stack_schema, "HealthStack");

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

    return BB_OK;
}
