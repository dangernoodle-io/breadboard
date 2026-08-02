#pragma once

/*
 * bb_wdt test hooks — host only, included when BB_WDT_TESTING is defined.
 *
 * Exposes counters for verifying WDT subscription and feed behavior in
 * host unit tests without requiring real ESP-IDF hardware.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reset all test counters to zero and clear any claimed-core state. Call at
 * the start of each test. */
void bb_wdt_test_reset(void);

/* Returns the number of times bb_wdt_task_feed() was called. */
int bb_wdt_test_feed_count(void);

/* Returns the number of times bb_wdt_task_subscribe() was called. */
int bb_wdt_test_subscribe_count(void);

/* Returns the number of times bb_wdt_task_unsubscribe() was called. */
int bb_wdt_test_unsubscribe_count(void);

/* Returns the handle passed to the most recent
 * bb_wdt_task_subscribe_handle() call (NULL for a self-subscribe). */
void *bb_wdt_test_last_subscribe_handle(void);

/* Returns the handle passed to the most recent
 * bb_wdt_task_unsubscribe_handle() call (NULL for a self-unsubscribe). */
void *bb_wdt_test_last_unsubscribe_handle(void);

/* Returns the idle-core mask actually passed to the most recent (stubbed)
 * reconfigure call -- what bb_wdt_set_timeout()/bb_wdt_claim_core()/
 * bb_wdt_release_core() genuinely computed and applied, not just what the
 * pure bb_wdt_derive_idle_mask() would return in isolation. This mask has
 * already been run through bb_wdt_clamp_idle_mask(), same as the ESP-IDF
 * backend's do_reconfigure() -- see bb_wdt_test_set_num_cores(). */
uint32_t bb_wdt_test_last_reconfigured_mask(void);

/* Test-only: set the simulated core count the host backend's reconfigure
 * path clamps against (mirrors the ESP-IDF backend's configNUMBER_OF_CORES
 * argument to bb_wdt_clamp_idle_mask()). Defaults to 2 (dual-core) on every
 * bb_wdt_test_reset(); set to 1 to reproduce a unicore (esp32-s2/-c3) target
 * and prove a core-1 claim never reaches bb_wdt_test_last_reconfigured_mask()
 * with an out-of-range bit set. */
void bb_wdt_test_set_num_cores(int num_cores);

#ifdef __cplusplus
}
#endif
