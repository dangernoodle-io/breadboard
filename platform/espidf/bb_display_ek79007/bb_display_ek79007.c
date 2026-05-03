#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_display_ek79007.h"

#include "bb_log.h"
#include "bb_hw.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ek79007.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "ek79007";

static esp_ldo_channel_handle_t s_ldo = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t *s_lv_disp = NULL;
static lv_obj_t *s_screen = NULL;

lv_obj_t *bb_display_ek79007_screen(void)
{
    return s_screen;
}

bool bb_display_ek79007_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bb_display_ek79007_unlock(void)
{
    lvgl_port_unlock();
}

static bb_err_t ek79007_init(uint16_t *width_out, uint16_t *height_out)
{
    esp_err_t err = ESP_OK;

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = LDO_CHANNEL,
        .voltage_mv = LDO_VOLTAGE_MV,
    };
    err = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo);
    if (err != ESP_OK) {
        bb_log_e(TAG, "failed to acquire LDO channel: %s", esp_err_to_name(err));
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
        bb_log_e(TAG, "failed to create DSI bus: %s", esp_err_to_name(err));
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
        bb_log_e(TAG, "failed to create panel IO: %s", esp_err_to_name(err));
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
        return err;
    }

    esp_lcd_dpi_panel_config_t dpi_cfg = EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_cfg.num_fbs = 2;

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
        bb_log_e(TAG, "failed to create panel: %s", esp_err_to_name(err));
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
        bb_log_e(TAG, "failed to reset panel: %s", esp_err_to_name(err));
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
        bb_log_e(TAG, "failed to init panel: %s", esp_err_to_name(err));
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

    (void)esp_lcd_panel_disp_on_off(s_panel, true);

    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << PIN_BL_PWR) | (1ULL << PIN_BL_PWM),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_BL_PWR, 1);
    gpio_set_level(PIN_BL_PWM, 1);

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        bb_log_e(TAG, "failed to init lvgl port: %s", esp_err_to_name(err));
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
            .direct_mode = true,
        },
    };
    lvgl_port_display_dsi_cfg_t dsi_disp_cfg = {
        .flags = {
            .avoid_tearing = true,
        },
    };
    s_lv_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_disp_cfg);
    if (s_lv_disp == NULL) {
        bb_log_e(TAG, "failed to add DSI display to LVGL");
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

    *width_out = PANEL_WIDTH;
    *height_out = PANEL_HEIGHT;

    bb_log_i(TAG, "init: %dx%d %s (LVGL)", PANEL_WIDTH, PANEL_HEIGHT, PANEL_NAME);

    return ESP_OK;
}

static void ek79007_clear(uint16_t rgb565)
{
    if (s_screen == NULL) {
        return;
    }

    uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;
    uint8_t g = ((rgb565 >> 5)  & 0x3F) << 2;
    uint8_t b = ( rgb565        & 0x1F) << 3;
    uint32_t rgb888 = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

    bb_display_ek79007_lock(0);
    lv_obj_clean(s_screen);
    static lv_style_t s_clear_style;
    lv_style_init(&s_clear_style);
    lv_style_set_bg_color(&s_clear_style, lv_color_hex(rgb888));
    lv_style_set_bg_opa(&s_clear_style, LV_OPA_COVER);
    lv_obj_add_style(s_screen, &s_clear_style, 0);
    lv_obj_invalidate(s_screen);
    bb_display_ek79007_unlock();
}

static void ek79007_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    if (s_panel == NULL) {
        return;
    }

    int16_t x1 = x;
    int16_t y1 = y;
    int16_t x2 = x + w - 1;
    int16_t y2 = y + h - 1;

    if (x2 < 0 || y2 < 0 || x1 >= PANEL_WIDTH || y1 >= PANEL_HEIGHT) {
        return;
    }

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= PANEL_WIDTH) x2 = PANEL_WIDTH - 1;
    if (y2 >= PANEL_HEIGHT) y2 = PANEL_HEIGHT - 1;

    uint16_t clipped_w = x2 - x1 + 1;
    uint16_t clipped_h = y2 - y1 + 1;

    if (clipped_w == 0 || clipped_h == 0) {
        return;
    }

    bb_display_ek79007_lock(0);
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, pixels);
    bb_display_ek79007_unlock();
}

static void ek79007_flush(void)
{
}

static void ek79007_off(void)
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

static void ek79007_draw_text(int16_t x, int16_t y, const char *text,
                               const bb_display_font_t *font,
                               uint16_t fg_rgb565, uint16_t bg_rgb565)
{
    if (s_screen == NULL) {
        return;
    }

    bb_display_ek79007_lock(0);

    uint8_t r = ((fg_rgb565 >> 11) & 0x1F) << 3;
    uint8_t g = ((fg_rgb565 >> 5)  & 0x3F) << 2;
    uint8_t b = ( fg_rgb565        & 0x1F) << 3;
    uint32_t fg_rgb888 = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

    lv_obj_t *lbl = lv_label_create(s_screen);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg_rgb888), 0);

    const lv_font_t *lv_font = NULL;
#if LV_FONT_MONTSERRAT_32
    if (font->glyph_h >= 28) {
        lv_font = &lv_font_montserrat_32;
    } else
#endif
#if LV_FONT_MONTSERRAT_24
    if (font->glyph_h >= 20) {
        lv_font = &lv_font_montserrat_24;
    } else
#endif
#if LV_FONT_MONTSERRAT_16
    if (font->glyph_h >= 12) {
        lv_font = &lv_font_montserrat_16;
    } else
#endif
    {
        lv_font = NULL;
    }

    if (lv_font != NULL) {
        lv_obj_set_style_text_font(lbl, lv_font, 0);
    }

    lv_obj_set_pos(lbl, x, y);

    bb_display_ek79007_unlock();
}

static const bb_display_backend_t s_backend = {
    .name         = "ek79007",
    .probe        = NULL,
    .init         = ek79007_init,
    .clear        = ek79007_clear,
    .blit         = ek79007_blit,
    .flush        = ek79007_flush,
    .off          = ek79007_off,
    .draw_text    = ek79007_draw_text,
    .set_rotation = NULL,  /* LVGL handles rotation; out of scope for this API */
};

#if CONFIG_BB_DISPLAY_EK79007_AUTOREGISTER
__attribute__((constructor))
static void bb_display_register__ek79007(void)
{
    bb_display_register_backend(&s_backend);
}
#endif
