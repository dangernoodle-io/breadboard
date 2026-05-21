#include "bb_display_spi_common.h"
#include "bb_log.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

static const char *TAG = "bb_display_spi";

bb_err_t bb_display_spi_init_bus(int pin_mosi, int pin_miso, int pin_clk,
                                 int max_transfer_sz, int host,
                                 int pclk_hz, int pin_cs, int pin_dc,
                                 esp_lcd_panel_io_handle_t *out_io)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num      = pin_mosi,
        .miso_io_num      = pin_miso,
        .sclk_io_num      = pin_clk,
        .quadwp_io_num    = -1,
        .quadhd_io_num    = -1,
        .max_transfer_sz  = max_transfer_sz,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        bb_log_e(TAG, "spi bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = pin_cs,
        .dc_gpio_num       = pin_dc,
        .spi_mode          = 0,
        .pclk_hz           = pclk_hz,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)(intptr_t)host, &io_cfg, out_io);
    if (err != ESP_OK) {
        bb_log_e(TAG, "panel io init failed: %s", esp_err_to_name(err));
        *out_io = NULL;
        return err;
    }

    return BB_OK;
}

void bb_display_blit_spi(esp_lcd_panel_handle_t panel,
                         int16_t x, int16_t y,
                         uint16_t w, uint16_t h,
                         const uint16_t *pixels)
{
    if (!panel || !pixels || !w || !h) return;

    enum { BOUNCE_PIXELS = 512 };
    static uint16_t bounce[BOUNCE_PIXELS];
    int16_t row = 0;
    while (row < (int16_t)h) {
        size_t rows_this_pass = BOUNCE_PIXELS / w;
        if (rows_this_pass == 0) rows_this_pass = 1;
        if ((size_t)(h - row) < rows_this_pass) rows_this_pass = (size_t)(h - row);
        size_t pixels_this_pass = rows_this_pass * w;
        if (pixels_this_pass > BOUNCE_PIXELS) pixels_this_pass = BOUNCE_PIXELS;
        for (size_t i = 0; i < pixels_this_pass; i++) {
            uint16_t c = pixels[row * w + i];
            bounce[i] = (uint16_t)((c >> 8) | (c << 8));
        }
        esp_lcd_panel_draw_bitmap(panel, x, y + row, x + w, y + row + (int16_t)rows_this_pass, bounce);
        row += (int16_t)rows_this_pass;
    }
}
