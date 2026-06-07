// bb_power_test.h — test hooks for bb_power. Only included when BB_POWER_TESTING is defined.
#pragma once

#ifdef BB_POWER_TESTING

#ifdef __cplusplus
extern "C" {
#endif

// Reset primary slot for test isolation.
void bb_power_test_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* BB_POWER_TESTING */
