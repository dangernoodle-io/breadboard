#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Test helper: read the last GPIO level set for a pin.
// Returns -1 if gpio is out of range.
int bb_led_gpio_host_get_level(int gpio);

#ifdef __cplusplus
}
#endif
