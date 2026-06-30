#include "bb_display_spi_common.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "bb_hw.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"

#include <string.h>

static const char *TAG = "st77xx";

esp_lcd_panel_io_handle_t bb_display_st77xx_panel_io = NULL;
esp_lcd_panel_handle_t    bb_display_st77xx_panel = NULL;

static bool s_bus_initialized = false;

// SPI2_HOST = 1 in spi_host_device_t; CONFIG default 1 = SPI2_HOST.
#define LCD_SPI_HOST    CONFIG_BB_DISPLAY_ST77XX_SPI_HOST
#define LCD_PIXEL_CLK   CONFIG_BB_DISPLAY_ST77XX_CLK_HZ

bb_err_t bb_display_st77xx_init_bus(void)
{
    if (s_bus_initialized) return BB_OK;

    bb_err_t err = bb_display_spi_init_bus(
        PIN_LCD_MOSI, /*pin_miso=*/-1, PIN_LCD_CLK,
        LCD_WIDTH * LCD_HEIGHT * 2,
        LCD_SPI_HOST,
        LCD_PIXEL_CLK,
        PIN_LCD_CS, PIN_LCD_DC,
        &bb_display_st77xx_panel_io);
    if (err != BB_OK) {
        bb_log_e(TAG, "init_bus failed");
        return err;
    }

    s_bus_initialized = true;
    return BB_OK;
}

void bb_display_st77xx_clear(uint16_t rgb565)
{
    if (!bb_display_st77xx_panel) return;

    /* Line-buffered approach: alloc one scanline in DMA-capable RAM, fill, blit
     * repeatedly. Hard fail on NULL — non-DMA memory would cause silent data
     * corruption on SPI DMA transfers. */
    uint16_t *line = bb_malloc_dma(LCD_WIDTH * sizeof(uint16_t));
    if (!line) {
        bb_log_w(TAG, "clear: DMA malloc failed for scanline");
        return;
    }

    for (uint16_t i = 0; i < LCD_WIDTH; i++) {
        line[i] = rgb565;
    }

    for (uint16_t y = 0; y < LCD_HEIGHT; y++) {
        esp_lcd_panel_draw_bitmap(bb_display_st77xx_panel, 0, y, LCD_WIDTH, y + 1, line);
    }

    bb_mem_free(line);
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

    bb_display_blit_spi(bb_display_st77xx_panel, x, y, w, h, pixels);
}

void bb_display_st77xx_off(void)
{
    if (!bb_display_st77xx_panel) return;

    /* Turn off backlight (active-low: set to 1). */
    gpio_set_level(PIN_LCD_BL, 1);

    /* Disable display. */
    esp_lcd_panel_disp_on_off(bb_display_st77xx_panel, false);
}

void bb_display_st77xx_on(void)
{
    if (!bb_display_st77xx_panel) return;

    /* Turn on backlight (active-low: set to 0). */
    gpio_set_level(PIN_LCD_BL, 0);

    /* Enable display. */
    esp_lcd_panel_disp_on_off(bb_display_st77xx_panel, true);
}
