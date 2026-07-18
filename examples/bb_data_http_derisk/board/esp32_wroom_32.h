#pragma once

// Classic ESP32-D0 / WROOM-32 module -- mirrors examples/smoke's own board
// header (bb_hw.h requires a BB_HW_BOARD_HEADER; this example is
// bench-flashed on a WROOM-32 pogopin board, so it needs one too). No
// display/peripheral pins -- this example touches none.

#define BOARD_NAME "esp32-wroom-32"

#define PIN_UART0_TX 1
#define PIN_UART0_RX 3

#define PIN_LED 2

#define FLASH_SIZE_MB 4
#define FLASH_MODE    "DIO"
