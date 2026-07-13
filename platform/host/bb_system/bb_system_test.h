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
