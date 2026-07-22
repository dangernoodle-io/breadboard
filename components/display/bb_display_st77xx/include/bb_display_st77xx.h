#pragma once
#include <stdint.h>
#include "bb_core.h"

/* Both backends use the consumer's bb_hw board header for pin and
 * dimension defines:
 *   PIN_LCD_CLK, PIN_LCD_MOSI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST,
 *   PIN_LCD_BL, LCD_WIDTH, LCD_HEIGHT, LCD_OFFSET_X, LCD_OFFSET_Y.
 *
 * Backlight is active-low on the LilyGo T-Dongle S3 (BL=0 → on).
 * If a future board has active-high BL, add a Kconfig knob.
 */
