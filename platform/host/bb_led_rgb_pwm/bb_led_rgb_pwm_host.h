#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Test helper: read the last effective duty for a GPIO pin (post active_low
// inversion), normalized to 0..65535 range (max_duty == 65535 on host).
// Returns -1 if pin is out of range [0,64) or has never been opened/written.
long bb_led_rgb_pwm_host_get_duty(int gpio);

// Test helper: read the raw 0..255 base color component stored for a GPIO pin.
// Returns -1 if out of range or never set.
int bb_led_rgb_pwm_host_get_base(int gpio);

// Test helper: read the last envelope (0..65535) applied to a GPIO pin.
// Returns -1 if out of range or never set.
long bb_led_rgb_pwm_host_get_env(int gpio);

// Reset all tracked pin state.
void bb_led_rgb_pwm_host_reset(void);

// Wrapper called by test_main.c setUp().
void bb_led_rgb_pwm_host_test_reset(void);

#ifdef __cplusplus
}
#endif
