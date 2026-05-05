#include "bb_log.h"
#include "bb_hw.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include <string.h>

static const char *TAG = "st77xx";

esp_lcd_panel_io_handle_t bb_display_st77xx_panel_io = NULL;
esp_lcd_panel_handle_t    bb_display_st77xx_panel = NULL;

static bool s_bus_initialized = false;

#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIXEL_CLK   20000000

bb_err_t bb_display_st77xx_init_bus(void)
{
    if (s_bus_initialized) return BB_OK;

    /* Configure SPI bus. */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        bb_log_e(TAG, "spi bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Configure panel I/O (SPI). */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLK,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                   &io_cfg, &bb_display_st77xx_panel_io);
    if (err != ESP_OK) {
        bb_log_e(TAG, "panel io init failed: %s", esp_err_to_name(err));
        bb_display_st77xx_panel_io = NULL;
        return err;
    }

    s_bus_initialized = true;
    return BB_OK;
}

void bb_display_st77xx_clear(uint16_t rgb565)
{
    if (!bb_display_st77xx_panel) return;

    /* Line-buffered approach: alloc one scanline, fill, blit repeatedly. */
    uint16_t *line = malloc(LCD_WIDTH * sizeof(uint16_t));
    if (!line) {
        bb_log_w(TAG, "clear: malloc failed for scanline");
        return;
    }

    for (uint16_t i = 0; i < LCD_WIDTH; i++) {
        line[i] = rgb565;
    }

    for (uint16_t y = 0; y < LCD_HEIGHT; y++) {
        esp_lcd_panel_draw_bitmap(bb_display_st77xx_panel, 0, y, LCD_WIDTH, y + 1, line);
    }

    free(line);
}

void bb_display_st77xx_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    if (!bb_display_st77xx_panel || !pixels || !w || !h) return;

    /* Panel offsets are applied in hardware via esp_lcd_panel_set_gap() at init —
     * do not add them here or they double up. */
    if (x < 0 || y < 0 || (uint16_t)x + w > LCD_WIDTH || (uint16_t)y + h > LCD_HEIGHT) {
        bb_log_w(TAG, "blit out of bounds: (%d,%d) %ux%u", x, y, w, h);
        return;
    }

    /* Byte-swap RGB565 into buffer (ESP LCD expects big-endian on SPI). */
    size_t pixel_count = (size_t)w * h;
    uint16_t *buf = NULL;

    if (pixel_count <= 256) {
        /* Small blits: use stack buffer. */
        uint16_t stack_buf[256];
        for (size_t i = 0; i < pixel_count; i++) {
            uint16_t pixel = pixels[i];
            /* Swap bytes: XRRRRR GGG | BBBBB XX -> GGG BBBBB | XX XRRRRR */
            stack_buf[i] = ((pixel & 0xFF) << 8) | ((pixel >> 8) & 0xFF);
        }
        esp_lcd_panel_draw_bitmap(bb_display_st77xx_panel, x, y, x + w, y + h, stack_buf);
    } else {
        /* Large blits: allocate heap buffer (bounded). */
        if (pixel_count > 8192) {
            bb_log_w(TAG, "blit too large: %zu pixels (max 8192)", pixel_count);
            return;
        }
        buf = malloc(pixel_count * sizeof(uint16_t));
        if (!buf) {
            bb_log_w(TAG, "blit malloc failed for %zu pixels", pixel_count);
            return;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            uint16_t pixel = pixels[i];
            buf[i] = ((pixel & 0xFF) << 8) | ((pixel >> 8) & 0xFF);
        }
        esp_lcd_panel_draw_bitmap(bb_display_st77xx_panel, x, y, x + w, y + h, buf);
        free(buf);
    }
}

void bb_display_st77xx_off(void)
{
    if (!bb_display_st77xx_panel) return;

    /* Turn off backlight (active-low: set to 1). */
    gpio_set_level(PIN_LCD_BL, 1);

    /* Disable display. */
    esp_lcd_panel_disp_on_off(bb_display_st77xx_panel, false);
}
