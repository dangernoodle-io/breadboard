// bb_pub_rtos — telemetry source satellite for FreeRTOS task-stack high-water-marks.
//
// Registers a bb_pub source under the "rtos" subtopic. On each tick, emits:
//   min_free_stack       integer (bytes) — smallest stack HWM across all
//                        APP tasks (benign system tasks excluded — see below)
//   min_free_stack_task  string  — name of the most-at-risk app task
//   task_count           integer — number of FreeRTOS tasks
//
// Named critical tasks (emitted only when the task exists, in bytes):
//   stack_bb_pub    — bb_pub worker task HWM
//   stack_httpd     — httpd task HWM
//   stack_mqtt      — esp-mqtt task HWM
//   stack_ipc0      — IPC core 0 task HWM
//   stack_ipc1      — IPC core 1 task HWM
//   stack_main      — main/app_main task HWM
//
// Self-registration is gated on CONFIG_BB_PUB_RTOS_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP tier
// at an order after bb_pub so the source registry exists first.
//
// Requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y (already enabled; /api/diag/tasks uses it).
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "rtos" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 * Called automatically at PRE_HTTP tier when CONFIG_BB_PUB_RTOS_AUTO_ATTACH=y.
 */
bb_err_t bb_pub_rtos_register(void);

/**
 * True for FreeRTOS system tasks with intentionally tiny, idle-heavy stacks
 * (ipc0/ipc1, IDLE*, esp_timer, Tmr Svc). Their high-water marks sit low by
 * design and are NOT an overflow risk, so they are excluded from the
 * min_free_stack headline (which should reflect APP tasks that can actually
 * overflow). They are still reported individually (e.g. stack_ipc0).
 * Exposed for host testing.
 */
bool bb_pub_rtos_is_benign_task(const char *name);

#ifdef __cplusplus
}
#endif
