#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_display_ili9341.h"
#include "bb_log.h"
#include "bb_hw.h"
#include "sdkconfig.h"

#include "driver/spi_master.h"
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

static bool spi_bus_init_once(void) {
    if (s_bus_inited) return true;
    spi_bus_config_t cfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .sclk_io_num = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ILI9341_NATIVE_W * ILI9341_NATIVE_H * 2,
    };
    int host = CONFIG_BB_DISPLAY_ILI9341_SPI_HOST;
    if (spi_bus_initialize((spi_host_device_t)host, &cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        bb_log_e(TAG, "spi bus init failed");
        return false;
    }
    s_bus_inited = true;
    return true;
}

static bool panel_io_init_once(void) {
    if (s_io) return true;
    if (!spi_bus_init_once()) return false;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_BB_DISPLAY_ILI9341_PIXEL_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    int host = CONFIG_BB_DISPLAY_ILI9341_SPI_HOST;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)(intptr_t)host, &io_cfg, &s_io) != ESP_OK) {
        bb_log_e(TAG, "panel io init failed");
        return false;
    }
    return true;
}

static bb_err_t ili9341_probe(void) {
    /* MISO must be wired for RDDID. If the board header sets PIN_LCD_MISO < 0,
     * we cannot probe — skip (registry walker treats NULL probe as success). */
    if (PIN_LCD_MISO < 0) return BB_ERR_INVALID_STATE;

    if (!panel_io_init_once()) return BB_ERR_INVALID_STATE;

    /* RDDID4 (0xD3): reads 4 bytes — leading dummy then ID1, ID2, ID3.
     * Expected: 0x00, 0x93, 0x41 in the trailing three. */
    uint8_t buf[4] = {0};
    if (esp_lcd_panel_io_rx_param(s_io, 0xD3, buf, sizeof(buf)) != ESP_OK) {
        return BB_ERR_NOT_FOUND;
    }
    /* Accept either layout — pad-byte-leading or compact. */
    bool match = (buf[1] == 0x00 && buf[2] == 0x93 && buf[3] == 0x41) ||
                 (buf[0] == 0x00 && buf[1] == 0x93 && buf[2] == 0x41) ||
                 (buf[2] == 0x93 && buf[3] == 0x41);
    if (!match) {
        bb_log_w(TAG, "RDDID mismatch: %02x %02x %02x %02x", buf[0], buf[1], buf[2], buf[3]);
        return BB_ERR_NOT_FOUND;
    }
    bb_log_i(TAG, "RDDID match: %02x %02x %02x %02x", buf[0], buf[1], buf[2], buf[3]);
    return BB_OK;
}

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
    static uint16_t s_line[ILI9341_NATIVE_H];
    uint16_t swapped = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
    for (int x = 0; x < ILI9341_NATIVE_H; x++) s_line[x] = swapped;
    for (int y = 0; y < ILI9341_NATIVE_W; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, ILI9341_NATIVE_H, y + 1, s_line);
    }
}

static void ili9341_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels) {
    if (!s_panel || !pixels || !w || !h) return;
    enum { BOUNCE_PIXELS = 512 };
    static uint16_t bounce[BOUNCE_PIXELS];
    int16_t row = 0;
    while (row < (int16_t)h) {
        size_t rows_this_pass = BOUNCE_PIXELS / w;
        if (rows_this_pass == 0) rows_this_pass = 1;
        if ((size_t)(h - row) < rows_this_pass) rows_this_pass = h - row;
        size_t pixels_this_pass = rows_this_pass * w;
        if (pixels_this_pass > BOUNCE_PIXELS) pixels_this_pass = BOUNCE_PIXELS;
        for (size_t i = 0; i < pixels_this_pass; i++) {
            uint16_t c = pixels[row * w + i];
            bounce[i] = (uint16_t)((c >> 8) | (c << 8));
        }
        esp_lcd_panel_draw_bitmap(s_panel, x, y + row, x + w, y + row + rows_this_pass, bounce);
        row += (int16_t)rows_this_pass;
    }
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

static const bb_display_backend_t s_backend = {
    .name      = "ili9341",
    .probe     = ili9341_probe,
    .init      = ili9341_init,
    .clear     = ili9341_clear,
    .blit      = ili9341_blit,
    .flush     = NULL,
    .off       = ili9341_off,
    .draw_text = NULL,
};

#if CONFIG_BB_DISPLAY_ILI9341_AUTOREGISTER
void bb_display_register__ili9341(void) __attribute__((constructor));
void bb_display_register__ili9341(void) {
    bb_display_register_backend(&s_backend);
}
#endif
