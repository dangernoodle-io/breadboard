#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Test-only: advance the mock clock. Only available when BB_BUTTON_EVENTS_MOCK_CLOCK
// is defined (set in the native test build via platformio.ini build_flags).
void bb_button_events_set_mock_time_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
