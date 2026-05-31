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
