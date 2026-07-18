#pragma once

// Classic ESP32-D0 / WROOM-32 module -- floor's only board target (board =
// esp32dev in platformio.ini). Minimal header: floor is the measurement
// rig, not a hardware-feature demo -- BOARD_NAME is the only field any
// composed component (bb_mdns's default hostname/instance seed) actually
// reads. Mirrors examples/smoke/board/esp32_wroom_32.h; extend here only if
// a future floor addition needs another BB_HW_* field.

#define BOARD_NAME "esp32-wroom-32"
