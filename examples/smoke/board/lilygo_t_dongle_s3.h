#pragma once

// LILYGO T-Dongle-S3 (ESP32-S3, USB-A male connector, onboard ST7735 LCD).
// Minimal smoke-test board header — pin set extends as components are exercised.

#define BOARD_NAME "lilygo-t-dongle-s3"

// USB-Serial-JTAG handles console + flash; no UART pin defines needed.

// Onboard ST7735 LCD (SPI) — exercised by bb_display_st77xx
#define PIN_LCD_CLK   5
#define PIN_LCD_MOSI  3
#define PIN_LCD_CS    4
#define PIN_LCD_DC    2
#define PIN_LCD_RST   1
#define PIN_LCD_BL    38
#define LCD_WIDTH     160
#define LCD_HEIGHT    80
#define LCD_OFFSET_X  1
#define LCD_OFFSET_Y  26

// Onboard MicroSD slot (TF card via SDIO):
//   CMD = 6, CLK = 7, D0 = 14, D1 = 15, D2 = 16, D3 = 17

// Flash and PSRAM
#define FLASH_SIZE_MB 16
#define FLASH_MODE    "QIO"
