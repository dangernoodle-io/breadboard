#pragma once

// Classic ESP32-D0 / WROOM-32 module (e.g., ELEGOO ESP32 dev board with CP2102 UART).
// Minimal smoke-test board header — pin set extends as components are exercised.

#define BOARD_NAME "esp32-wroom-32"

// UART0 routes to the CP2102 USB-UART bridge on most WROOM-32 dev boards.
#define PIN_UART0_TX 1
#define PIN_UART0_RX 3

// Onboard LED on GPIO2 for many WROOM-32 dev boards (active high).
#define PIN_LED 2

// Flash configuration
#define FLASH_SIZE_MB 4
#define FLASH_MODE    "DIO"
