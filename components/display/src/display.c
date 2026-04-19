#include "display.h"

#include "esp_log.h"
#include "board.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ek79007.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "display";

static esp_ldo_channel_handle_t s_ldo = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t *s_lv_disp = NULL;
static lv_obj_t *s_screen = NULL;

bb_display_err_t bb_display_init(void)
{
    esp_err_t err = ESP_OK;

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = LDO_CHANNEL,
        .voltage_mv = LDO_VOLTAGE_MV,
    };
    err = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to acquire LDO channel: %s", esp_err_to_name(err));
        return err;
    }

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

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };

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

    // Initialize LVGL port.
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to init lvgl port: %s", esp_err_to_name(err));
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

    // Register DSI display with LVGL.
    // avoid_tearing uses internal DSI double-buffer as LVGL draw buffers (full-screen, PSRAM).
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .control_handle = NULL,
        .buffer_size = PANEL_WIDTH * PANEL_HEIGHT,
        .double_buffer = true,
        .hres = PANEL_WIDTH,
        .vres = PANEL_HEIGHT,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    lvgl_port_display_dsi_cfg_t dsi_disp_cfg = {
        .flags = {
            .avoid_tearing = true,
        },
    };
    s_lv_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_disp_cfg);
    if (s_lv_disp == NULL) {
        ESP_LOGE(TAG, "failed to add DSI display to LVGL");
        lvgl_port_deinit();
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
        return ESP_FAIL;
    }

    s_screen = lv_display_get_screen_active(s_lv_disp);

    ESP_LOGI(TAG, "init: %dx%d %s (LVGL)", PANEL_WIDTH, PANEL_HEIGHT, PANEL_NAME);

    return ESP_OK;
}

lv_obj_t *bb_display_screen(void)
{
    return s_screen;
}

bool bb_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bb_display_unlock(void)
{
    lvgl_port_unlock();
}

void bb_display_clear(uint16_t rgb565)
{
    if (s_screen == NULL) {
        return;
    }

    // Expand RGB565 to RGB888.
    uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;
    uint8_t g = ((rgb565 >> 5)  & 0x3F) << 2;
    uint8_t b = ( rgb565        & 0x1F) << 3;
    uint32_t rgb888 = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

    bb_display_lock(0);
    static lv_style_t s_clear_style;
    lv_style_init(&s_clear_style);
    lv_style_set_bg_color(&s_clear_style, lv_color_hex(rgb888));
    lv_style_set_bg_opa(&s_clear_style, LV_OPA_COVER);
    lv_obj_add_style(s_screen, &s_clear_style, 0);
    lv_obj_invalidate(s_screen);
    bb_display_unlock();
}

void bb_display_show_splash(const char *product, const char *version)
{
    if (s_screen == NULL) {
        return;
    }

    bb_display_lock(0);

    lv_obj_clean(s_screen);

    static lv_style_t s_splash_bg;
    lv_style_init(&s_splash_bg);
    lv_style_set_bg_color(&s_splash_bg, lv_color_hex(0x000020));
    lv_style_set_bg_opa(&s_splash_bg, LV_OPA_COVER);
    lv_obj_add_style(s_screen, &s_splash_bg, 0);

    lv_obj_t *lbl_product = lv_label_create(s_screen);
    lv_label_set_text(lbl_product, product ? product : "");
#if LV_FONT_MONTSERRAT_32
    lv_obj_set_style_text_font(lbl_product, &lv_font_montserrat_32, 0);
#endif
    lv_obj_set_style_text_color(lbl_product, lv_color_white(), 0);
    lv_obj_align(lbl_product, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *lbl_version = lv_label_create(s_screen);
    lv_label_set_text(lbl_version, version ? version : "");
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(lbl_version, &lv_font_montserrat_16, 0);
#endif
    lv_obj_set_style_text_color(lbl_version, lv_color_hex(0x888888), 0);
    lv_obj_align(lbl_version, LV_ALIGN_CENTER, 0, 20);

    bb_display_unlock();
}

void bb_display_show_prov(const char *ap_ssid, const char *ap_pass)
{
    if (s_screen == NULL) {
        return;
    }

    bb_display_lock(0);

    lv_obj_clean(s_screen);

    static lv_style_t s_prov_bg;
    lv_style_init(&s_prov_bg);
    lv_style_set_bg_color(&s_prov_bg, lv_color_hex(0x001A00));
    lv_style_set_bg_opa(&s_prov_bg, LV_OPA_COVER);
    lv_obj_add_style(s_screen, &s_prov_bg, 0);

    lv_obj_t *lbl_title = lv_label_create(s_screen);
    lv_label_set_text(lbl_title, "Provisioning");
#if LV_FONT_MONTSERRAT_32
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_32, 0);
#endif
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *lbl_ssid = lv_label_create(s_screen);
    lv_label_set_text(lbl_ssid, ap_ssid ? ap_ssid : "");
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(lbl_ssid, &lv_font_montserrat_16, 0);
#endif
    lv_obj_set_style_text_color(lbl_ssid, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(lbl_ssid, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *lbl_pass = lv_label_create(s_screen);
    lv_label_set_text(lbl_pass, ap_pass ? ap_pass : "");
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(lbl_pass, &lv_font_montserrat_16, 0);
#endif
    lv_obj_set_style_text_color(lbl_pass, lv_color_hex(0x888888), 0);
    lv_obj_align(lbl_pass, LV_ALIGN_CENTER, 0, 40);

    bb_display_unlock();
}

void bb_display_off(void)
{
    if (s_lv_disp != NULL) {
        lvgl_port_remove_disp(s_lv_disp);
        s_lv_disp = NULL;
        s_screen = NULL;
    }

    lvgl_port_deinit();

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
}
