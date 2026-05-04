#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Test-only: advance the mock clock. Only available when BB_LED_ANIM_MOCK_CLOCK
// is defined (set in the native test build via native_scaffold / build_flags).
void bb_led_anim_set_mock_time_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
