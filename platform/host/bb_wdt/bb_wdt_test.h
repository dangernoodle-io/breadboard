#pragma once

/*
 * bb_wdt test hooks — host only, included when BB_WDT_TESTING is defined.
 *
 * Exposes counters for verifying WDT subscription and feed behavior in
 * host unit tests without requiring real ESP-IDF hardware.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Reset all test counters to zero. Call at the start of each test. */
void bb_wdt_test_reset(void);

/* Returns the number of times bb_wdt_task_feed() was called. */
int bb_wdt_test_feed_count(void);

/* Returns the number of times bb_wdt_task_subscribe() was called. */
int bb_wdt_test_subscribe_count(void);

/* Returns the number of times bb_wdt_task_unsubscribe() was called. */
int bb_wdt_test_unsubscribe_count(void);

#ifdef __cplusplus
}
#endif
