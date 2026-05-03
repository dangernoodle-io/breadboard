#pragma once
#include <stdint.h>
#include "bb_core.h"

/* Arduino backend using direct SPI (no Adafruit deps).
 * Consumer board header provides pin defines:
 *   PIN_LCD_CLK, PIN_LCD_MOSI, PIN_LCD_MISO, PIN_LCD_CS, PIN_LCD_DC,
 *   PIN_LCD_RST (optional, set to -1 if no hw reset),
 *   PIN_LCD_BL  (optional, set to -1 if backlight isn't software-controlled).
 *
 * Native panel dimensions are 240x320 (portrait); backend reports
 * landscape (320x240) after rotation. RDDID probe (cmd 0xD3) reads
 * 4 bytes; expects 0x93, 0x41 signature.
 */
