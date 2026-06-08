#include "bb_display_spi_common.h"
#include "bb_log.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

static const char *TAG = "bb_display_spi";

bb_err_t bb_display_spi_init_bus_only(int pin_mosi, int pin_miso, int pin_clk,
                                       int max_transfer_sz, int host)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = pin_mosi,
        .miso_io_num     = pin_miso,
        .sclk_io_num     = pin_clk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = max_transfer_sz,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        bb_log_e(TAG, "spi bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    return BB_OK;
}

bb_err_t bb_display_spi_new_panel_io(int host, int pclk_hz, int pin_cs, int pin_dc,
                                      esp_lcd_panel_io_handle_t *out_io)
{
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = pin_cs,
        .dc_gpio_num       = pin_dc,
        .spi_mode          = 0,
        .pclk_hz           = pclk_hz,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    esp_err_t err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)(intptr_t)host, &io_cfg, out_io);
    if (err != ESP_OK) {
        bb_log_e(TAG, "panel io init failed: %s", esp_err_to_name(err));
        *out_io = NULL;
        return err;
    }
    return BB_OK;
}

/* Back-compat wrapper — existing callers (st77xx) keep building unchanged. */
bb_err_t bb_display_spi_init_bus(int pin_mosi, int pin_miso, int pin_clk,
                                  int max_transfer_sz, int host,
                                  int pclk_hz, int pin_cs, int pin_dc,
                                  esp_lcd_panel_io_handle_t *out_io)
{
    bb_err_t err = bb_display_spi_init_bus_only(
        pin_mosi, pin_miso, pin_clk, max_transfer_sz, host);
    if (err != BB_OK) return err;
    return bb_display_spi_new_panel_io(host, pclk_hz, pin_cs, pin_dc, out_io);
}

enum { BOUNCE_PIXELS = 512 };
static uint16_t s_bounce[BOUNCE_PIXELS];

void bb_display_blit_spi(esp_lcd_panel_handle_t panel,
                         int16_t x, int16_t y,
                         uint16_t w, uint16_t h,
                         const uint16_t *pixels)
{
    if (!panel || !pixels || !w || !h) return;

    int16_t row = 0;
    while (row < (int16_t)h) {
        size_t rows_this_pass = BOUNCE_PIXELS / w;
        if (rows_this_pass == 0) rows_this_pass = 1;
        if ((size_t)(h - row) < rows_this_pass) rows_this_pass = (size_t)(h - row);
        size_t pixels_this_pass = rows_this_pass * w;
        if (pixels_this_pass > BOUNCE_PIXELS) pixels_this_pass = BOUNCE_PIXELS;
        for (size_t i = 0; i < pixels_this_pass; i++) {
            uint16_t c = pixels[row * w + i];
            s_bounce[i] = (uint16_t)((c >> 8) | (c << 8));
        }
        esp_lcd_panel_draw_bitmap(panel, x, y + row, x + w, y + row + (int16_t)rows_this_pass, s_bounce);
        row += (int16_t)rows_this_pass;
    }
}

void bb_display_clear_spi(esp_lcd_panel_handle_t panel,
                          uint16_t x, uint16_t y,
                          uint16_t w, uint16_t h,
                          uint16_t rgb565_swapped)
{
    if (!panel || !w || !h) return;

    /* Fill bounce buffer with the solid color once; reuse for every scanline. */
    size_t fill = w < BOUNCE_PIXELS ? w : BOUNCE_PIXELS;
    for (size_t i = 0; i < fill; i++) s_bounce[i] = rgb565_swapped;

    for (uint16_t row = 0; row < h; row++) {
        esp_lcd_panel_draw_bitmap(panel, x, y + row, x + w, y + row + 1, s_bounce);
    }
}
