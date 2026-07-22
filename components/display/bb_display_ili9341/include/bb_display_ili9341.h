#pragma once
#include <stdint.h>
#include "bb_core.h"

/* This backend uses the consumer's bb_hw board header for pin defines:
 *   PIN_LCD_CLK, PIN_LCD_MOSI, PIN_LCD_MISO, PIN_LCD_CS, PIN_LCD_DC,
 *   PIN_LCD_RST (set to -1 if no hw reset line is wired),
 *   PIN_LCD_BL  (set to -1 if backlight isn't software-controlled).
 *
 * Native panel dimensions are 240x320 (portrait); the backend reports
 * landscape (320x240) after applying madctl rotation. Override via
 * Kconfig later if portrait orientation is wanted by a consumer.
 *
 * RDDID probe (cmd 0xD3) reads 4 bytes; expects bytes [1..3] to be
 * 0x00, 0x93, 0x41 (or the lower 16 bits to equal 0x9341, since some
 * clones omit the leading 0x00s). PIN_LCD_MISO must be wired and not
 * -1 for probe to run; backends without MISO get probe=NULL behavior
 * (always-succeed) — fall back to that if the board header reports
 * PIN_LCD_MISO == -1.
 */
