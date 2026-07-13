// bb_fan_test.h — test hooks for bb_fan. Only included when BB_FAN_TESTING is defined.
#pragma once

#ifdef BB_FAN_TESTING

#include "bb_fan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reset primary slot for test isolation.
void bb_fan_test_reset(void);

#ifdef CONFIG_BB_FAN_AUTOFAN
// Inject a mock clock for the PID (ms timestamp). Null = use real clock.
void bb_fan_pid_set_mock_clock(bb_fan_handle_t h, unsigned long (*fn)(void));

// Invoke the registered autofan persist callback (used by host test handlers).
void bb_fan_invoke_autofan_persist_cb(const bb_fan_autofan_cfg_t *cfg);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BB_FAN_TESTING */
