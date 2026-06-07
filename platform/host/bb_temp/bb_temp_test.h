#pragma once

#include <stdbool.h>

/**
 * Host-only test hook: inject a simulated SoC temperature reading.
 * When present is true, bb_temp_read_soc writes celsius and returns true.
 * When present is false, bb_temp_read_soc returns false (*out untouched).
 * Default state: present=false (mimics unsupported silicon).
 * Only available when BB_TEMP_TESTING is defined.
 */
#ifdef BB_TEMP_TESTING
void bb_temp_test_set_soc(bool present, float celsius);
#endif
