#pragma once

// bb_task_registry — thin consumer of the generic bb_registry primitive that
// tracks every self-registered FreeRTOS task under a diagnostic name, for
// surfacing at GET /api/diag/tasks (bb_diag) and the "rtos" bb_pub telemetry
// source (bb_pub_rtos).
//
// OBSERVABILITY ONLY (B1-445). This component does NOT subscribe tasks to the
// Task WDT, does NOT alter scheduling, and does NOT change stack sizes or
// task priorities. Automatic WDT subscription is a separate, deferred
// feature (B1-458) — do not add it here.
//
// Call bb_task_registry_register() once, immediately after a successful
// xTaskCreate*() at each task-creation site, passing the stack budget already
// known at that call site (bytes) and the resulting task handle (opaque
// void* — TaskHandle_t on ESP-IDF). Best-effort: a full registry
// (BB_TASK_REGISTRY_MAX) or a duplicate name is logged and does NOT fail
// task creation. `handle` may be NULL when the site does not retain one.
//
// Call bb_task_registry_deregister() before a task's own vTaskDelete(NULL)
// if the task self-deletes (e.g. a one-shot worker) so the registry does not
// retain a stale entry for a task that no longer exists.
//
// This header contains no FreeRTOS types — the task handle is carried as an
// opaque void* — keeping it portable per project convention.

#include <stdint.h>
#include <stdbool.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BB_TASK_REGISTRY_NAME_MAX 20

// Register a task under `name`, recording its stack budget (bytes) and
// opaque handle. Best-effort — see file header.
// Returns BB_ERR_INVALID_ARG if name is NULL.
// Returns BB_ERR_NO_SPACE if the registry is full.
// Returns BB_ERR_INVALID_STATE on a duplicate name.
// `name` must be <= configMAX_TASK_NAME_LEN (typically 16) to match the
// kernel-truncated name reported by the /api/diag/tasks lookup; longer names
// still register but will not correlate against a live TaskStatus_t entry.
bb_err_t bb_task_registry_register(const char *name, uint32_t stack_budget_bytes, void *handle);

// Deregister a previously-registered task by handle value. Resolves the
// registered name internally (scans the registry for a matching handle) so
// callers do not need to track the name separately.
// Returns BB_ERR_INVALID_ARG if handle is NULL.
// Returns BB_ERR_NOT_FOUND if handle was never registered (e.g. registration
// failed at creation time due to a full registry, or handle was NULL at
// registration time).
//
// Note: a stale entry left behind by a task that never called deregister
// before self-deleting could, in principle, be matched first if FreeRTOS
// later reuses the freed TaskHandle_t for a new task. This is acceptable
// today because every self-deleting task in-tree calls deregister
// immediately before vTaskDelete(NULL), so no stale entry with a live
// (reusable) handle should exist in practice.
bb_err_t bb_task_registry_deregister(void *handle);

// Current registered task count.
uint16_t bb_task_registry_count(void);

// Pure lookup — the host-testable seam consumed by the /api/diag/tasks
// handler (bb_diag) and the "rtos" bb_pub telemetry source (bb_pub_rtos).
// Returns true and fills *out_budget (if non-NULL) and *out_wdt (if
// non-NULL) when `name` is registered; returns false otherwise (out params
// left untouched).
bool bb_task_registry_lookup_budget(const char *name, uint32_t *out_budget, bool *out_wdt);

// Iterate all registered tasks. Holds the internal lock across the ENTIRE
// call, including every invocation of cb — mirrors bb_ring_registry_foreach's
// contract (see bb_ring_registry.h for the full rationale: this prevents a
// concurrent deregister from invalidating an entry mid-read).
//
// Foreach contract — cb MUST be:
//   - bounded (finite, fast — it runs while the registry is fully locked)
//   - allocation-free
//   - a pure snapshot/copy-out of name/stack_budget_bytes/wdt_subscribed
//   - MUST NOT perform I/O of any kind
//   - non-blocking otherwise too (no other locks, no waiting)
//   - MUST NOT call back into any bb_task_registry_* function (register,
//     deregister, foreach, count, lookup_budget, test_reset) — the lock is
//     not recursive and doing so self-deadlocks.
typedef void (*bb_task_registry_cb_t)(const char *name, uint32_t stack_budget_bytes,
                                       bool wdt_subscribed, void *ctx);
void bb_task_registry_foreach(bb_task_registry_cb_t cb, void *ctx);

#ifdef BB_TASK_REGISTRY_TESTING
// Reset the registry to its initial (empty) state. Test teardown only.
void bb_task_registry_test_reset(void);

// Seed hook: register `name`/`stack_budget_bytes`/`wdt_subscribed` directly,
// without a real task handle. Host tests have no FreeRTOS TaskHandle_t to
// pass, and wdt_subscribed cannot be queried from a real WDT on host — this
// bypasses both, storing the caller-supplied wdt_subscribed value verbatim.
bb_err_t bb_task_registry_test_seed(const char *name, uint32_t stack_budget_bytes, bool wdt_subscribed);
#endif

#ifdef __cplusplus
}
#endif
