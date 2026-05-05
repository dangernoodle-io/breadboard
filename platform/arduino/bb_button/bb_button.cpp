// bb_button dispatch layer — Arduino C++ shim; logic lives in platform/host/bb_button/bb_button.c.
#ifdef ARDUINO

#include <Arduino.h>

extern "C" {
#include "../../host/bb_button/bb_button.c"
} // extern "C"

#endif // ARDUINO
