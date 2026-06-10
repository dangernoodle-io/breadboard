#pragma once

/*
 * bb_wdt — portable Task Watchdog Timer management.
 *
 * Consolidates live Task-WDT management (timeout reconfiguration,
 * task subscription, periodic feeding, and the WDT-friendly poll loop)
 * into a single component. ESP-IDF backend wraps esp_task_wdt_*; host
 * backend is no-op (with test-observable counters when BB_WDT_TESTING).
 *
 * Portability: this header contains no ESP-IDF or FreeRTOS types.
 */

#include "bb_core.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reconfigure the Task WDT timeout, preserving the idle-core mask and
 * panic-on-timeout policy from sdkconfig. ESP-IDF only; no-op on host.
 */
void bb_wdt_set_timeout(uint32_t timeout_s);

/*
 * Extend the Task WDT timeout to extended_s for a long blocking operation
 * (e.g. OTA flash). Mirrors bb_wdt_set_timeout(extended_s).
 */
void bb_wdt_extend_begin(uint32_t extended_s);

/*
 * Restore the Task WDT timeout to CONFIG_ESP_TASK_WDT_TIMEOUT_S.
 * Call after a long operation to return to the normal WDT window.
 */
void bb_wdt_extend_end(void);

/*
 * Subscribe the calling task to the Task WDT (esp_task_wdt_add(NULL)).
 * Returns BB_OK on success, or a platform error code on failure.
 * No-op / returns BB_OK on host.
 */
bb_err_t bb_wdt_task_subscribe(void);

/*
 * Unsubscribe the calling task from the Task WDT (esp_task_wdt_delete(NULL)).
 * Returns BB_OK on success, BB_OK if not subscribed, or a platform error.
 * No-op / returns BB_OK on host.
 */
bb_err_t bb_wdt_task_unsubscribe(void);

/*
 * Feed the Task WDT for the calling task (esp_task_wdt_reset(NULL)).
 * No-op on host.
 */
void bb_wdt_task_feed(void);

/*
 * Portable WDT-friendly park.
 *
 * Removes the calling task from the Task WDT, blocks on try_wait(ctx, total_ms),
 * then re-adds the task and feeds it. Used to park a WDT-subscribed task (e.g.
 * mining/asic) across a long cooperative pause (OTA flash phase): on a single
 * core the parked task cannot get scheduled to feed, so it must leave the WDT
 * rather than try to feed it. The active task's own subscription plus the OTA
 * timeout extension cover the system while parked.
 *
 * Parameters:
 *   try_wait  — blocking predicate; returns true when the park ends (resume)
 *   ctx       — opaque context passed through to try_wait
 *   total_ms  — total wait budget in milliseconds (passed straight to try_wait)
 *   slice_ms  — retained for ABI compatibility; ignored
 *
 * Returns try_wait's result: true if resumed within total_ms, false on timeout.
 * Assumes the caller is a WDT-subscribed task.
 */
bool bb_wdt_park_wait(bool (*try_wait)(void *ctx, uint32_t ms),
                      void *ctx,
                      uint32_t total_ms,
                      uint32_t slice_ms);

#ifdef __cplusplus
}
#endif
