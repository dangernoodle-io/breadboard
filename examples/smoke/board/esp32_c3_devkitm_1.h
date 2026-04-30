#pragma once

// Espressif ESP32-C3-DevKitM-1 (RISC-V, 4 MB flash, USB-Serial-JTAG console).
// Minimal smoke-test board header — pin set extends as components are exercised.

#define BOARD_NAME "esp32-c3-devkitm-1"

// USB-Serial-JTAG handles console + flash; no UART pin defines needed.

// Onboard RGB LED on GPIO8 (WS2812).
#define PIN_LED 8

// Flash configuration
#define FLASH_SIZE_MB 4
#define FLASH_MODE    "DIO"
