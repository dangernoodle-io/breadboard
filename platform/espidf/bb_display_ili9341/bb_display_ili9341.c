#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_display_autoregister.h"
#include "bb_display_ili9341.h"
#include "bb_display_spi_common.h"
#include "bb_log.h"
#include "bb_hw.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "nvs.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"

#include <string.h>

static const char *TAG = "bb_display_ili9341";

#define ILI9341_NATIVE_W 240
#define ILI9341_NATIVE_H 320

static esp_lcd_panel_io_handle_t s_io    = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;
static bool                      s_bus_inited = false;

/* Init the SPI bus once. Does NOT create s_io. */
static bool bus_init_once(void) {
    if (s_bus_inited) return true;
    int host = CONFIG_BB_DISPLAY_ILI9341_SPI_HOST;
    bb_err_t err = bb_display_spi_init_bus_only(
        PIN_LCD_MOSI, PIN_LCD_MISO, PIN_LCD_CLK,
        ILI9341_NATIVE_W * ILI9341_NATIVE_H * 2,
        host);
    if (err != BB_OK) return false;
    s_bus_inited = true;
    return true;
}

/* Create the 40 MHz pixel IO handle (used by init + blit). */
static bool panel_io_init_once(void) {
    if (s_io) return true;
    if (!bus_init_once()) return false;
    int host = CONFIG_BB_DISPLAY_ILI9341_SPI_HOST;
    bb_err_t err = bb_display_spi_new_panel_io(
        host,
        CONFIG_BB_DISPLAY_ILI9341_PIXEL_CLK_HZ,
        PIN_LCD_CS, PIN_LCD_DC,
        &s_io);
    return (err == BB_OK);
}

#if !CONFIG_BB_DISPLAY_ILI9341_SKIP_PROBE
static bb_err_t ili9341_probe(void) {
    if (PIN_LCD_MISO < 0) return BB_ERR_INVALID_STATE;

    /* Ensure the SPI bus is up. Does NOT create s_io. */
    if (!bus_init_once()) return BB_ERR_INVALID_STATE;

    /* Temporary low-clock IO handle for RDDID: the ILI9341 cannot drive MISO
     * for register reads at the 40 MHz pixel clock. Deleted before return. */
    int host = CONFIG_BB_DISPLAY_ILI9341_SPI_HOST;
    esp_lcd_panel_io_handle_t probe_io = NULL;
    bb_err_t err = bb_display_spi_new_panel_io(
        host,
        CONFIG_BB_DISPLAY_ILI9341_READ_CLK_HZ,
        PIN_LCD_CS, PIN_LCD_DC,
        &probe_io);
    if (err != BB_OK) return BB_ERR_INVALID_STATE;

    /* RDDID4 (0xD3): 4 bytes — leading dummy then ID1, ID2, ID3. */
    uint8_t buf[4] = {0};
    esp_err_t rx_err = esp_lcd_panel_io_rx_param(probe_io, 0xD3, buf, sizeof(buf));

    esp_lcd_panel_io_del(probe_io);  /* always free the temp handle */

    if (rx_err != ESP_OK) return BB_ERR_NOT_FOUND;

    bool match = (buf[1] == 0x00 && buf[2] == 0x93 && buf[3] == 0x41) ||
                 (buf[0] == 0x00 && buf[1] == 0x93 && buf[2] == 0x41) ||
                 (buf[2] == 0x93 && buf[3] == 0x41);
    if (!match) {
        bb_log_w(TAG, "RDDID mismatch: %02x %02x %02x %02x",
                 buf[0], buf[1], buf[2], buf[3]);
        return BB_ERR_NOT_FOUND;
    }
    bb_log_i(TAG, "RDDID match: %02x %02x %02x %02x",
             buf[0], buf[1], buf[2], buf[3]);
    return BB_OK;
}
#endif

static bb_err_t ili9341_init(uint16_t *w, uint16_t *h) {
    if (!panel_io_init_once()) return BB_ERR_INVALID_STATE;

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        bb_log_e(TAG, "panel init failed");
        return BB_ERR_INVALID_STATE;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    if (PIN_LCD_BL >= 0) {
        gpio_set_direction(PIN_LCD_BL, GPIO_MODE_OUTPUT);
#if CONFIG_BB_DISPLAY_ILI9341_BL_ACTIVE_LOW
        gpio_set_level(PIN_LCD_BL, 0);
#else
        gpio_set_level(PIN_LCD_BL, 1);
#endif
    }

    *w = ILI9341_NATIVE_H;
    *h = ILI9341_NATIVE_W;
    bb_log_i(TAG, "ready %ux%u", (unsigned)*w, (unsigned)*h);
    return BB_OK;
}

static void ili9341_clear(uint16_t rgb565) {
    if (!s_panel) return;
    uint16_t swapped = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
    bb_display_clear_spi(s_panel, 0, 0, ILI9341_NATIVE_H, ILI9341_NATIVE_W, swapped);
}

static void ili9341_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels) {
    bb_display_blit_spi(s_panel, x, y, w, h, pixels);
}

static void ili9341_off(void) {
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, false);
    }
    if (PIN_LCD_BL >= 0) {
#if CONFIG_BB_DISPLAY_ILI9341_BL_ACTIVE_LOW
        gpio_set_level(PIN_LCD_BL, 1);
#else
        gpio_set_level(PIN_LCD_BL, 0);
#endif
    }
}

static void ili9341_on(void) {
    if (!s_panel) return;
    esp_lcd_panel_disp_on_off(s_panel, true);
    if (PIN_LCD_BL >= 0) {
#if CONFIG_BB_DISPLAY_ILI9341_BL_ACTIVE_LOW
        gpio_set_level(PIN_LCD_BL, 0);
#else
        gpio_set_level(PIN_LCD_BL, 1);
#endif
    }
}

static bb_err_t ili9341_set_rotation(uint16_t deg, uint16_t *w, uint16_t *h)
{
    if (!s_panel) return BB_ERR_INVALID_STATE;

    bool swap_xy, mirror_x, mirror_y;
    uint16_t width = ILI9341_NATIVE_H, height = ILI9341_NATIVE_W;  /* default landscape */

    switch (deg) {
        case 0:
            swap_xy = false;
            mirror_x = false;
            mirror_y = false;
            width = ILI9341_NATIVE_W;
            height = ILI9341_NATIVE_H;
            break;
        case 90:
            swap_xy = true;
            mirror_x = true;
            mirror_y = false;
            width = ILI9341_NATIVE_H;
            height = ILI9341_NATIVE_W;
            break;
        case 180:
            swap_xy = false;
            mirror_x = true;
            mirror_y = true;
            width = ILI9341_NATIVE_W;
            height = ILI9341_NATIVE_H;
            break;
        case 270:
            swap_xy = true;
            mirror_x = false;
            mirror_y = true;
            width = ILI9341_NATIVE_H;
            height = ILI9341_NATIVE_W;
            break;
        default:
            return BB_ERR_INVALID_ARG;
    }

    if (esp_lcd_panel_swap_xy(s_panel, swap_xy) != ESP_OK) {
        return BB_ERR_INVALID_STATE;
    }
    if (esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y) != ESP_OK) {
        return BB_ERR_INVALID_STATE;
    }

    *w = width;
    *h = height;
    return BB_OK;
}

static const bb_display_backend_t s_backend = {
    .name         = "ili9341",
#if !CONFIG_BB_DISPLAY_ILI9341_SKIP_PROBE
    .probe        = ili9341_probe,
#endif
    .init         = ili9341_init,
    .clear        = ili9341_clear,
    .blit         = ili9341_blit,
    .flush        = NULL,
    .off          = ili9341_off,
    .on           = ili9341_on,
    .draw_text    = NULL,
    .set_rotation = ili9341_set_rotation,
};

BB_DISPLAY_AUTOREGISTER(ili9341, CONFIG_BB_DISPLAY_ILI9341_AUTOREGISTER, &s_backend)
