// bb_button_events — Arduino C++ shim; logic lives in
// platform/host/bb_button_events/bb_button_events.c.
// auto_start_timer is unsupported on Arduino; call bb_button_events_tick()
// from loop().
#ifdef ARDUINO

#include <Arduino.h>

extern "C" {
#include "../../host/bb_button_events/bb_button_events.c"
} // extern "C"

#endif // ARDUINO
