#pragma once

// LILYGO T-Dongle-S3 (ESP32-S3, USB-A male connector, onboard ST7735 LCD).
// Minimal smoke-test board header — pin set extends as components are exercised.
// LCD is not initialized in the unified smoke (see B1-28 for panel abstraction).

#define BOARD_NAME "lilygo-t-dongle-s3"

// USB-Serial-JTAG handles console + flash; no UART pin defines needed.

// Onboard ST7735 LCD pins (reserved for future display work — not used today):
//   MOSI = 3, SCLK = 5, CS = 4, DC = 2, RST = 1, BL = 38

// Onboard MicroSD slot (TF card via SDIO):
//   CMD = 6, CLK = 7, D0 = 14, D1 = 15, D2 = 16, D3 = 17

// Flash and PSRAM
#define FLASH_SIZE_MB 16
#define FLASH_MODE    "QIO"
