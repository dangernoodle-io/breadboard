#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_log.h"
#include "bb_hw.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"

static const char *TAG = "st7789";

extern esp_lcd_panel_io_handle_t bb_display_st77xx_panel_io;
extern esp_lcd_panel_handle_t    bb_display_st77xx_panel;
bb_err_t bb_display_st77xx_init_bus(void);
void     bb_display_st77xx_clear(uint16_t);
void     bb_display_st77xx_blit(int16_t, int16_t, uint16_t, uint16_t, const uint16_t *);
void     bb_display_st77xx_off(void);

static bb_err_t st7789_init(uint16_t *width_out, uint16_t *height_out)
{
    /* Initialize SPI bus and panel I/O. */
    bb_err_t err = bb_display_st77xx_init_bus();
    if (err != BB_OK) {
        bb_log_e(TAG, "init_bus failed");
        return err;
    }

    /* Configure backlight GPIO (active-low: 1 = off during init). */
    gpio_set_direction(PIN_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_BL, 1);

    /* Create panel handle. */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_err_t esp_err = esp_lcd_new_panel_st7789(bb_display_st77xx_panel_io, &panel_cfg,
                                                 &bb_display_st77xx_panel);
    if (esp_err != ESP_OK) {
        bb_log_e(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(esp_err));
        return esp_err;
    }

    /* Reset and initialize panel. */
    esp_lcd_panel_reset(bb_display_st77xx_panel);
    esp_lcd_panel_init(bb_display_st77xx_panel);

    /* Panel config. */
    esp_lcd_panel_set_gap(bb_display_st77xx_panel, LCD_OFFSET_X, LCD_OFFSET_Y);
    esp_lcd_panel_mirror(bb_display_st77xx_panel, false, true);
    esp_lcd_panel_disp_on_off(bb_display_st77xx_panel, true);

    /* Turn on backlight (active-low: 0 = on). */
    gpio_set_level(PIN_LCD_BL, 0);

    bb_log_i(TAG, "st7789 ready");
    *width_out = LCD_WIDTH;
    *height_out = LCD_HEIGHT;
    return BB_OK;
}

static bb_err_t st7789_set_rotation(uint16_t deg, uint16_t *w, uint16_t *h)
{
    if (!bb_display_st77xx_panel) return BB_ERR_INVALID_STATE;

    bool swap_xy, mirror_x, mirror_y;
    uint16_t width = LCD_WIDTH, height = LCD_HEIGHT;

    switch (deg) {
        case 0:
            swap_xy = false;
            mirror_x = false;
            mirror_y = false;
            width = LCD_WIDTH;
            height = LCD_HEIGHT;
            break;
        case 90:
            swap_xy = true;
            mirror_x = true;
            mirror_y = false;
            width = LCD_HEIGHT;
            height = LCD_WIDTH;
            break;
        case 180:
            swap_xy = false;
            mirror_x = true;
            mirror_y = true;
            width = LCD_WIDTH;
            height = LCD_HEIGHT;
            break;
        case 270:
            swap_xy = true;
            mirror_x = false;
            mirror_y = true;
            width = LCD_HEIGHT;
            height = LCD_WIDTH;
            break;
        default:
            return BB_ERR_INVALID_ARG;
    }

    if (esp_lcd_panel_swap_xy(bb_display_st77xx_panel, swap_xy) != ESP_OK) {
        return BB_ERR_INVALID_STATE;
    }
    if (esp_lcd_panel_mirror(bb_display_st77xx_panel, mirror_x, mirror_y) != ESP_OK) {
        return BB_ERR_INVALID_STATE;
    }

    *w = width;
    *h = height;
    return BB_OK;
}

static const bb_display_backend_t s_backend = {
    .name         = "st7789",
    .probe        = NULL,
    .init         = st7789_init,
    .clear        = bb_display_st77xx_clear,
    .blit         = bb_display_st77xx_blit,
    .flush        = NULL,
    .off          = bb_display_st77xx_off,
    .draw_text    = NULL,
    .set_rotation = st7789_set_rotation,
};

void bb_display_register__st7789(void) __attribute__((constructor));
void bb_display_register__st7789(void)
{
    bb_display_register_backend(&s_backend);
}
