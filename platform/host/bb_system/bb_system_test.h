#pragma once

#include "bb_core.h"

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
