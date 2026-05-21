#pragma once
#include <stdint.h>
#include "bb_core.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_display_spi_common — shared SPI init and blit helpers.
 *
 * Private to bb_display_spi_common component; not part of any public
 * bb_display header surface. Drivers include this via relative path or
 * PRIV_INCLUDE_DIRS.
 *
 * bb_display_spi_init_bus: initialize the SPI bus and create a panel IO
 * handle.  Callers that guard their own "already inited" flag call this
 * once and skip on subsequent calls.
 *
 * bb_display_blit_spi: byte-swap RGB565 pixels through a 512-pixel
 * bounce buffer and push to the panel via esp_lcd_panel_draw_bitmap.
 * Eliminates the per-driver 1024-byte BSS bounce array.
 */

bb_err_t bb_display_spi_init_bus(int pin_mosi, int pin_miso, int pin_clk,
                                 int max_transfer_sz, int host,
                                 int pclk_hz, int pin_cs, int pin_dc,
                                 esp_lcd_panel_io_handle_t *out_io);

void bb_display_blit_spi(esp_lcd_panel_handle_t panel,
                         int16_t x, int16_t y,
                         uint16_t w, uint16_t h,
                         const uint16_t *pixels);

#ifdef __cplusplus
}
#endif
