#include "display.h"

#include "esp_log.h"
#include "board.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ek79007.h"
#include "driver/gpio.h"
#include "esp_cache.h"
#include "string.h"

static const char *TAG = "display";

// File-scope state
static esp_ldo_channel_handle_t s_ldo = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;
static size_t s_fb_size = 0;

bb_display_err_t bb_display_init(void)
{
    esp_err_t err = ESP_OK;

    // Acquire MIPI PHY LDO
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = LDO_CHANNEL,
        .voltage_mv = LDO_VOLTAGE_MV,
    };
    err = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to acquire LDO channel: %s", esp_err_to_name(err));
        return err;
    }

    // Build DSI bus config
    esp_lcd_dsi_bus_config_t dsi_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = 900,
    };
    err = esp_lcd_new_dsi_bus(&dsi_cfg, &s_dsi_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create DSI bus: %s", esp_err_to_name(err));
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
        return err;
    }

    // Build panel IO config
    esp_lcd_dbi_io_config_t panel_io_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_dbi(s_dsi_bus, &panel_io_cfg, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create panel IO: %s", esp_err_to_name(err));
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
        return err;
    }

    esp_lcd_dpi_panel_config_t dpi_cfg = EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);

    ek79007_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num = PANEL_LANES,
        },
        .init_cmds = NULL,
        .init_cmds_size = 0,
    };

    // Build panel config
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };

    // Create panel
    err = esp_lcd_new_panel_ek79007(s_panel_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create panel: %s", esp_err_to_name(err));
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
        return err;
    }

    // Initialize panel
    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to reset panel: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
        return err;
    }

    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to init panel: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
        return err;
    }

    // EK79007 DPI panel is always on after init; disp_on_off is not supported.
    (void)esp_lcd_panel_disp_on_off(s_panel, true);

    // Enable backlight power + PWM (drive both high for full brightness).
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << PIN_BL_PWR) | (1ULL << PIN_BL_PWM),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_BL_PWR, 1);
    gpio_set_level(PIN_BL_PWM, 1);

    // Get framebuffer
    err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, (void **)&s_fb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to get frame buffer: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
        return err;
    }

    s_fb_size = PANEL_WIDTH * PANEL_HEIGHT * 2;

    ESP_LOGI(TAG, "init: %dx%d %s (fb:%p size:%zu)", PANEL_WIDTH, PANEL_HEIGHT, PANEL_NAME, s_fb, s_fb_size);

    return ESP_OK;
}

void bb_display_clear(uint16_t rgb565)
{
    if (s_fb == NULL) {
        return;
    }

    for (size_t i = 0; i < (PANEL_WIDTH * PANEL_HEIGHT); i++) {
        s_fb[i] = rgb565;
    }
    esp_cache_msync(s_fb, s_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

void bb_display_draw_text(int x, int y, const char *text)
{
    // Stub: logging only
    (void)x;
    (void)y;
    ESP_LOGD(TAG, "draw_text at (%d,%d): %s", x, y, text ? text : "");
}

void bb_display_show_splash(const char *product, const char *version)
{
    ESP_LOGI(TAG, "splash: %s %s", product ? product : "", version ? version : "");
    bb_display_clear(0x001F);  // Blue
}

void bb_display_show_prov(const char *ap_ssid, const char *ap_pass)
{
    ESP_LOGI(TAG, "prov: ssid=%s pass=%s", ap_ssid ? ap_ssid : "", ap_pass ? ap_pass : "");
    bb_display_clear(0xF800);  // Red
}

void bb_display_off(void)
{
    if (s_panel != NULL) {
        esp_lcd_panel_disp_on_off(s_panel, false);
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }

    if (s_panel_io != NULL) {
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
    }

    if (s_dsi_bus != NULL) {
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
    }

    if (s_ldo != NULL) {
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
    }

    s_fb = NULL;
    s_fb_size = 0;
}
