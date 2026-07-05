#pragma once
// Private shared header: pure stack-monitor decision logic + JSON builder.
// No ESP-IDF or FreeRTOS types. Included by:
//   - platform/espidf/bb_health/bb_health_stack.c (monitor impl)
//   - platform/host/bb_health/bb_health_stack_host.c (stub + test harness)
//   - components/bb_health/bb_health_stack_common.c (pure impl)

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "bb_core.h"

#define BB_HEALTH_STACK_TOPIC "health.stack"

// Returns true if free_bytes is below threshold (stack is low).
bool bb_health_stack_is_low(uint32_t free_bytes, uint32_t threshold);

// Write JSON payload for a health.stack event into buf[buf_sz].
// Format: {"task":"<name>","free_bytes":<n>,"low":<bool>}
// Returns the number of characters that would have been written (like snprintf),
// -1 on invalid args.
int bb_health_stack_build_json(char *buf, size_t buf_sz,
                               const char *task_name,
                               uint32_t free_bytes,
                               bool low);

// ---------------------------------------------------------------------------
// Debounce table: mark-and-sweep (B1-601)
// ---------------------------------------------------------------------------
//
// The debounce table below is keyed by task NAME (not handle) and bounded by
// a fixed capacity. Short-lived pool tasks (e.g. sse_N) cycle through names,
// so an insert-only table leaks a slot per name forever. Mark-and-sweep
// bounds it to currently-live tasks: every poll cycle tags each live task's
// entry with the current scan tick (mark), then frees any entry whose tick
// doesn't match (sweep) -- its task is gone.
//
// Pure (no FreeRTOS types) so it is host-testable; the ESP-IDF poll loop in
// platform/espidf/bb_health/bb_health_stack.c owns the actual FreeRTOS
// snapshot and calls these against its own table.

#define BB_HEALTH_STACK_TASK_NAME_MAX 32

typedef struct {
    char     name[BB_HEALTH_STACK_TASK_NAME_MAX];
    bool     low;
    bool     in_use;
    uint32_t seen_tick;
} bb_health_stack_entry_t;

// Mark phase: find the entry for `name` in `table[cap]` and tag it with
// `scan_tick`, or insert it into a free slot if not present. Returns the
// entry pointer, or NULL if `name` is new and no free slot is available
// (table is at capacity with all-distinct live tasks).
bb_health_stack_entry_t *bb_health_stack_table_mark(bb_health_stack_entry_t *table, int cap,
                                                    const char *name, uint32_t scan_tick);

// Sweep phase: free entries that have been absent for more than
// BB_HEALTH_STACK_SWEEP_GRACE consecutive scans. A task transiently missing
// from a single uxTaskGetSystemState snapshot keeps its entry; only persistent
// absence across multiple scans triggers removal. Returns the number of entries freed.
int bb_health_stack_table_sweep(bb_health_stack_entry_t *table, int cap, uint32_t scan_tick);

// PRE_HTTP: starts the periodic stack high-water monitor timer. No topic/
// event/openapi side effects. No-op (BB_OK) when
// CONFIG_FREERTOS_USE_TRACE_FACILITY=n. Implemented in
// platform/espidf/bb_health/bb_health_stack.c; self-registers at PRE_HTTP
// when CONFIG_BB_HEALTH_STACK_AUTOSTART=y.
bb_err_t bb_health_stack_monitor_start(void);

// ---------------------------------------------------------------------------
// Test hook (BB_HEALTH_TESTING only)
// ---------------------------------------------------------------------------

#ifdef BB_HEALTH_TESTING
#ifdef __cplusplus
extern "C" {
#endif

// Simulate a poll cycle with synthetic task data. Drives the same
// debounce + event-post path used on ESP-IDF without requiring FreeRTOS.
// task_name: task name string; free_bytes: simulated free stack bytes;
// threshold: low threshold; already_low: previous low state for this task.
// Returns the new low state (true if low was posted, i.e. transition into low).
bool bb_health_stack_simulate_post(const char *task_name, uint32_t free_bytes,
                                   uint32_t threshold, bool already_low);

// Reset debounce state table (call from test setUp).
void bb_health_stack_reset_for_test(void);

// Count how many health.stack events have been posted via the host stub.
int bb_health_stack_post_count_for_test(void);

#ifdef __cplusplus
}
#endif
#endif /* BB_HEALTH_TESTING */
