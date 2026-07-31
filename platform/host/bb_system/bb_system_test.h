#pragma once

#include "bb_core.h"
#include "bb_lock.h"    // bb_lock_t
#include "bb_system.h"  // bb_reset_source_t
#include "bb_timer.h"   // bb_oneshot_timer_t

/**
 * Host-only test hook: inject a simulated die-temperature reading.
 * When rc == BB_OK, bb_system_read_temp_celsius writes celsius and returns BB_OK.
 * Any other rc is returned directly and *out is untouched.
 * Default state: rc = BB_ERR_UNSUPPORTED (mimics unsupported silicon).
 * Only available when BB_SYSTEM_TESTING is defined.
 */
void bb_system_set_temp_for_test(float celsius, bb_err_t rc);

/**
 * Host-only test hook: reset the in-memory boot-fail counter (B1-753) to 0.
 * s_boot_count is process-lifetime state; call from setUp() to prevent
 * cross-test leakage. Only available when BB_SYSTEM_TESTING is defined.
 */
void bb_system_boot_count_reset_for_test(void);

/**
 * Host-only test hook (B1-863): clear the reboot budget's per-cause
 * lazy-load cache (s_cache/s_loaded in bb_system_reboot_budget.c). That
 * cache is process-lifetime state -- a value loaded (or recorded) by one
 * test would otherwise leak into the next test for the same cause. Call
 * from setUp() / a reset helper to prevent cross-test leakage. Only
 * available when BB_SYSTEM_TESTING is defined.
 */
void bb_system_reboot_budget_reset_for_test(void);

/**
 * Host-only test hook (B1-1148 finding 1): reset the POST /api/reboot
 * handler's resolved-value capture (see
 * bb_system_reboot_capture_get_for_test()) to "not called". s_reboot_capture
 * is process-lifetime state; call from setUp() to prevent cross-test
 * leakage. Only available when BB_SYSTEM_TESTING is defined.
 */
void bb_system_reboot_capture_reset_for_test(void);

/**
 * Host-only test hook (B1-1148 finding 1): the (detail, ts) pair
 * reboot_handler() (platform/espidf/bb_system/bb_system_routes.c) resolved
 * on its most recent invocation -- captured immediately before the
 * #ifdef ESP_PLATFORM restart call the handler makes on device, which never
 * runs on host and is otherwise the only consumer of these values. out_detail
 * is written NUL-terminated within out_detail_size (truncated if necessary);
 * either out param may be NULL to skip it. Returns false if the handler has
 * not run since the last reset (out params untouched). Reusable by PR2's
 * parse-result assertions. Only available when BB_SYSTEM_TESTING is defined.
 */
bool bb_system_reboot_capture_get_for_test(char *out_detail, size_t out_detail_size, uint32_t *out_ts);

/**
 * Host-only test hook: reset bb_system_restart_deferred's capture (see
 * bb_system_restart_deferred_capture_get_for_test) to "not called".
 * s_restart_deferred_capture is process-lifetime state; call from setUp()
 * to prevent cross-test leakage. Only available when BB_SYSTEM_TESTING is
 * defined.
 */
void bb_system_restart_deferred_capture_reset_for_test(void);

/**
 * Host-only test hook: the (src, detail) pair bb_system_restart_deferred's
 * fired work_fn resolved on its most recent invocation -- captured instead
 * of the actual bb_system_restart_reason() call, which is ESP_PLATFORM-only
 * (on host it would exit(0) the test process; see restart_deferred_work_fn
 * in bb_system_restart_deferred.c). out_detail is written NUL-terminated
 * within out_detail_size (truncated if necessary); either out param may be
 * NULL to skip it. Returns false if the work_fn has not fired since the
 * last reset (out params untouched). Only available when BB_SYSTEM_TESTING
 * is defined.
 */
bool bb_system_restart_deferred_capture_get_for_test(bb_reset_source_t *out_src,
                                                       char *out_detail, size_t out_detail_size);

/**
 * Host-only test hook: the shared static bb_oneshot_timer_t
 * bb_system_restart_deferred lazily creates on first call (NULL before the
 * first call in this process). Lets a test fire it synchronously via
 * bb_timer_deferred_oneshot_fire_for_test (platform/host/bb_timer/
 * bb_timer_deferred_test.h) without bb_system_restart_deferred.c exposing
 * its own fire helper. Only available when BB_SYSTEM_TESTING is defined.
 */
bb_oneshot_timer_t bb_system_restart_deferred_timer_for_test(void);

/**
 * Host-only test hook: the shared static bb_lock_t bb_system_restart_deferred
 * uses to guard s_pending_src/s_pending_detail against concurrent
 * arm()/fire() callers (see bb_system.h's bb_system_restart_deferred doc
 * comment). Lazily bb_lock_init()'d on first call (never NULL on success).
 * Lets a test enable bb_lock stats (bb_lock_stats_set_enabled) and assert
 * bb_lock_get_stats' acquisition_count to prove the lock is genuinely taken
 * on both the arm and fire paths -- not just present in source. Only
 * available when BB_SYSTEM_TESTING is defined.
 */
bb_lock_t *bb_system_restart_deferred_pending_lock_for_test(void);
