// bb_led dispatch layer — Arduino C++ shim; logic lives in platform/host/bb_led/bb_led.c.
#ifdef ARDUINO

#include <Arduino.h>

extern "C" {
#include "../../host/bb_led/bb_led.c"
#include "../../host/bb_led/bb_led_gamma.c"
} // extern "C"

#endif // ARDUINO
