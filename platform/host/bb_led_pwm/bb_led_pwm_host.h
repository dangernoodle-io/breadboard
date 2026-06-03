#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Test helper: read the last brightness pct set for a pin.
// Returns -1 if pin is out of range or has never been opened.
int bb_led_pwm_host_get_pct(int gpio);

// Test helper: read the last value set via bb_led_set_level, after CIE gamma +
// active_low (0..65535). Returns -1 if out of range / never set.
long bb_led_pwm_host_get_level(int gpio);

// Test reset: clear all tracked state
void bb_led_pwm_test_reset(void);

#ifdef __cplusplus
}
#endif
