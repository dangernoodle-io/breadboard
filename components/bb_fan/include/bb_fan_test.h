// bb_fan_test.h — test hooks for bb_fan. Only included when BB_FAN_TESTING is defined.
#pragma once

#ifdef BB_FAN_TESTING

#ifdef __cplusplus
extern "C" {
#endif

// Reset primary slot for test isolation.
void bb_fan_test_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* BB_FAN_TESTING */
