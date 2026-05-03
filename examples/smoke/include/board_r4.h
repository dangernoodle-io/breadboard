#pragma once
/* Arduino UNO R4 WiFi smoke board header.
 *
 * NOTE: the Seeed 2.8" TFT Touch Shield V1.0 (5A12) we have on hand uses
 * an 8-bit PARALLEL ST7781R panel — none of breadboard's current SPI
 * backends (ili9341 / ssd1306 / st77xx) can drive it. Backlight lights,
 * but no command/pixel traffic ever reaches the controller. Pin defines
 * below are placeholders for a future SPI TFT hookup. */

#define PIN_LCD_CS    10
#define PIN_LCD_DC     9
#define PIN_LCD_RST    8
#define PIN_LCD_BL    -1
#define PIN_LCD_MISO  12
#define PIN_LCD_MOSI  11
#define PIN_LCD_CLK   13
