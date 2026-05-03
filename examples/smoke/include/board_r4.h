#pragma once
/* Seeed 2.8" TFT Touch Shield v1.0 (5A12) on Arduino UNO R4 Minima.
 * Hardware SPI on the ICSP header (MISO/MOSI/SCK = 12/11/13). */

#define PIN_LCD_CS    10
#define PIN_LCD_DC     9
#define PIN_LCD_RST    8
#define PIN_LCD_BL    -1   /* not software-controlled on this shield */
#define PIN_LCD_MISO  12   /* required for RDDID probe */
#define PIN_LCD_MOSI  11
#define PIN_LCD_CLK   13
