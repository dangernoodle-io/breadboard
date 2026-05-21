#pragma once
#include <stdbool.h>

// Private helper shared across platform implementations.
// LOW==0, HIGH==1 on Arduino, so the numeric form is portable.
static inline int bb_led_gpio_level_for_on(bool active_low, bool on) {
    return active_low ? (on ? 0 : 1) : (on ? 1 : 0);
}
