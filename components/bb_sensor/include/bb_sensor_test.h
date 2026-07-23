// bb_sensor test-only reset hook (BB_SENSOR_TESTING-gated).
#pragma once

#ifdef BB_SENSOR_TESTING

#ifdef __cplusplus
extern "C" {
#endif

// Reset state for test isolation: resets the underlying bb_temp SoC mock +
// bb_fan/bb_power test harnesses (single SSOT for every bb_sensor test).
void bb_sensor_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* BB_SENSOR_TESTING */
