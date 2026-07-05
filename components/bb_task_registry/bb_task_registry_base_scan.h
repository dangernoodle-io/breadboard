#pragma once
// bb_task_registry — pure (host-testable) periodic base-scan evaluator
// (task-registry unification PR3). No FreeRTOS types here -- portable,
// compiled host + ESP-IDF as part of the bb_task_registry component.
//
// Drives the reconciliation of bb_task's pointer-keyed base registry
// (components/bb_task) against a caller-supplied snapshot of currently-live
// tasks (populated by the ESP-IDF shell, platform/espidf/bb_task_registry/
// bb_task_registry_base_scan.c, from uxTaskGetSystemState), and detects
// per-task low-stack transitions for an optional observer (bb_health_stack,
// via bb_task_registry_set_low_stack_handler() in bb_task_registry.h).
//
// Private header: not under include/, not part of bb_task_registry's public
// surface. Shared only between this component's own pure impl and its
// ESP-IDF shell.

#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"
#include "bb_task_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BB_TASK_REGISTRY_SCAN_NAME_MAX 20  // mirrors BB_TASK_NAME_MAX (bb_task.h)

// One row per live task this scan, populated by the ESP-IDF shell from
// uxTaskGetSystemState. No FreeRTOS types here -- TaskHandle_t is itself an
// opaque void* on FreeRTOS.
typedef struct {
    void     *handle;
    char      name[BB_TASK_REGISTRY_SCAN_NAME_MAX];
    uint32_t  free_bytes;  // current stack high-water free bytes
} bb_task_registry_base_row_t;

// Runs one base-scan pass over `rows[n]` (this scan's live-task snapshot) at
// `now_tick`:
//   1. For each row: bb_task_base_touch_or_insert(handle, name, now_tick) --
//      a single atomic call that touches an already-tracked handle or
//      inserts a placeholder (0, false) for a new one -- the scan never
//      overwrites the budget/wdt_arm of an already-tracked handle (those
//      are owned by bb_task_create()/bb_task_registry_register()'s
//      overlay-join), and cannot lose a race to a concurrent creator
//      inserting the real entry mid-scan (see bb_task_base_touch_or_insert,
//      components/bb_task/include/bb_task.h).
//   2. Low-stack transition check per row (debounced by handle, own
//      grace-windowed table), firing the registered handler (see
//      bb_task_registry_set_low_stack_handler in bb_task_registry.h) once
//      per transition into low. No-op when no handler is registered.
//   3. Snapshots bb_task's base registry, runs bb_task_base_sweep_apply(),
//      and calls bb_task_base_remove() for every handle it reclaimed.
// Step 3 always runs, even when rows is NULL or n <= 0 (a scan that finds
// zero live tasks must still reclaim previously-tracked handles once their
// grace window expires) -- only steps 1-2 are skipped in that case.
// Returns the count of base entries reclaimed by the sweep (step 3).
int bb_task_registry_base_scan_apply(const bb_task_registry_base_row_t *rows, int n,
                                      uint32_t now_tick);

#ifdef BB_TASK_REGISTRY_TESTING
// Test-only: reset the low-stack debounce table and clear the handler.
void bb_task_registry_base_scan_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
