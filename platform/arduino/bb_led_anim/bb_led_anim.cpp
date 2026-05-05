// bb_led_anim — Arduino C++ shim; logic lives in platform/host/bb_led_anim/bb_led_anim.c.
// auto_start_timer is unsupported on Arduino; call bb_led_anim_tick() from loop().
#ifdef ARDUINO

#include <Arduino.h>

extern "C" {
#include "../../host/bb_led_anim/bb_led_anim.c"
} // extern "C"

#endif // ARDUINO
