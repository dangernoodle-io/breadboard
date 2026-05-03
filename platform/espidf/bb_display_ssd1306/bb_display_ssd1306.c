#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_display_ssd1306.h"
#include "bb_log.h"
#include "bb_hw.h"
#include "sdkconfig.h"

#include "driver/i2c_master.h"
#include "nvs.h"  /* BB_ERR_NOT_FOUND expands to ESP_ERR_NVS_NOT_FOUND */
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"

#include <string.h>

static const char *TAG = "bb_display_ssd1306";

#define SSD1306_WIDTH  128
#define SSD1306_HEIGHT CONFIG_BB_DISPLAY_SSD1306_HEIGHT
#define SSD1306_PAGES  (SSD1306_HEIGHT / 8)
#define SSD1306_FB_SZ  (SSD1306_WIDTH * SSD1306_PAGES)

static i2c_master_bus_handle_t s_user_bus = NULL;   /* override via setter */
static i2c_master_bus_handle_t s_owned_bus = NULL;  /* created by us if no override */
static esp_lcd_panel_io_handle_t s_io     = NULL;
static esp_lcd_panel_handle_t    s_panel  = NULL;
static uint8_t                   s_fb[SSD1306_FB_SZ];

void bb_display_ssd1306_set_i2c_bus(i2c_master_bus_handle_t bus)
{
    s_user_bus = bus;
}

static i2c_master_bus_handle_t resolve_bus(void)
{
    if (s_user_bus) return s_user_bus;
    if (s_owned_bus) return s_owned_bus;
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&cfg, &s_owned_bus) != ESP_OK) {
        s_owned_bus = NULL;
        return NULL;
    }
    return s_owned_bus;
}

static bb_err_t ssd1306_probe(void)
{
    i2c_master_bus_handle_t bus = resolve_bus();
    if (!bus) return BB_ERR_INVALID_STATE;
    /* i2c_master_probe pings the address; ESP_OK = device acked, ESP_ERR_NOT_FOUND = NACK. */
    esp_err_t err = i2c_master_probe(bus, CONFIG_BB_DISPLAY_SSD1306_I2C_ADDR, /*xfer_timeout_ms=*/50);
    if (err == ESP_OK) return BB_OK;
    return BB_ERR_NOT_FOUND;
}

static bb_err_t ssd1306_init(uint16_t *w, uint16_t *h)
{
    i2c_master_bus_handle_t bus = resolve_bus();
    if (!bus) return BB_ERR_INVALID_STATE;

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = CONFIG_BB_DISPLAY_SSD1306_I2C_ADDR,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .scl_speed_hz = CONFIG_BB_DISPLAY_SSD1306_I2C_HZ,
    };
    if (esp_lcd_new_panel_io_i2c_v2(bus, &io_cfg, &s_io) != ESP_OK) {
        bb_log_e(TAG, "i2c io init failed");
        return BB_ERR_INVALID_STATE;
    }

    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = SSD1306_HEIGHT };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
        .vendor_config = &ssd_cfg,
    };
    if (esp_lcd_new_panel_ssd1306(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        bb_log_e(TAG, "panel init failed");
        return BB_ERR_INVALID_STATE;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    memset(s_fb, 0, sizeof(s_fb));
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, s_fb);

    *w = SSD1306_WIDTH;
    *h = SSD1306_HEIGHT;
    bb_log_i(TAG, "ready %ux%u @ 0x%02X", (unsigned)SSD1306_WIDTH, (unsigned)SSD1306_HEIGHT,
             CONFIG_BB_DISPLAY_SSD1306_I2C_ADDR);
    return BB_OK;
}

static void set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) return;
    int page = y / 8;
    int bit  = y % 8;
    uint8_t mask = (uint8_t)(1u << bit);
    if (on) s_fb[page * SSD1306_WIDTH + x] |=  mask;
    else    s_fb[page * SSD1306_WIDTH + x] &= ~mask;
}

/* RGB565 → 1bpp via luma threshold. The previous "any non-zero = on" was too
 * aggressive: near-black backgrounds like 0x0004 mapped to ON and washed the
 * whole screen. Use a ~50% luma cutoff so callers can pass real RGB565 colors
 * intended for color panels and still get sensible mono rendering. */
static inline bool rgb565_to_mono(uint16_t c) {
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >> 5)  & 0x3F;
    uint8_t b = c & 0x1F;
    return (r * 2 + g + b * 2) > 90;
}

static void ssd1306_clear(uint16_t rgb565)
{
    memset(s_fb, rgb565_to_mono(rgb565) ? 0xFF : 0x00, sizeof(s_fb));
    if (s_panel) esp_lcd_panel_draw_bitmap(s_panel, 0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, s_fb);
}

static void ssd1306_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    if (!pixels || !s_panel) return;
    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            set_pixel(x + col, y + row, rgb565_to_mono(pixels[row * w + col]));
        }
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, s_fb);
}

static void ssd1306_off(void)
{
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
    /* Don't tear down the I²C bus we may have created — caller might re-init.
     * If we owned the bus, leave it; bb_display_init's next attempt will reuse
     * s_owned_bus. */
}

static bb_err_t ssd1306_set_rotation(uint16_t deg, uint16_t *w, uint16_t *h)
{
    if (!s_panel) return BB_ERR_INVALID_STATE;

    if (deg == 0) {
        if (esp_lcd_panel_mirror(s_panel, false, false) != ESP_OK) {
            return BB_ERR_INVALID_STATE;
        }
    } else if (deg == 180) {
        if (esp_lcd_panel_mirror(s_panel, true, true) != ESP_OK) {
            return BB_ERR_INVALID_STATE;
        }
    } else {
        return BB_ERR_INVALID_STATE;  /* 90/270 not supported on page-oriented FB */
    }

    /* dimensions don't change */
    *w = SSD1306_WIDTH;
    *h = SSD1306_HEIGHT;
    return BB_OK;
}

static const bb_display_backend_t s_backend = {
    .name         = "ssd1306",
    .probe        = ssd1306_probe,
    .init         = ssd1306_init,
    .clear        = ssd1306_clear,
    .blit         = ssd1306_blit,
    .flush        = NULL,            /* every blit/clear pushes immediately */
    .off          = ssd1306_off,
    .draw_text    = NULL,            /* core rasterizes via font + blit */
    .set_rotation = ssd1306_set_rotation,
};

#if CONFIG_BB_DISPLAY_SSD1306_AUTOREGISTER
void bb_display_register__ssd1306(void) __attribute__((constructor));
void bb_display_register__ssd1306(void) {
    bb_display_register_backend(&s_backend);
}
#endif
