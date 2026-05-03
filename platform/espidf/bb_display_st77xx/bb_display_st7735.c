#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_log.h"
#include "sdkconfig.h"
#include "bb_hw.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"

static const char *TAG = "st7735";

extern esp_lcd_panel_io_handle_t bb_display_st77xx_panel_io;
extern esp_lcd_panel_handle_t    bb_display_st77xx_panel;
bb_err_t bb_display_st77xx_init_bus(void);
void     bb_display_st77xx_clear(uint16_t);
void     bb_display_st77xx_blit(int16_t, int16_t, uint16_t, uint16_t, const uint16_t *);
void     bb_display_st77xx_off(void);

static esp_err_t st7735_vendor_init(void)
{
    /* Vendor init sequence (gamma / power / frame rate) for the
     * LilyGo T-Dongle S3 ST7735 panel. The stock esp_lcd ST7789
     * driver gets the basic command set right but doesn't program
     * these registers; without this overlay the panel comes up
     * with wrong colors and inverted gamma. */

    /* Frame rate control (normal/idle/partial) */
    uint8_t frmctr1[] = {0x05, 0x3A, 0x3A};
    uint8_t frmctr2[] = {0x05, 0x3A, 0x3A};
    uint8_t frmctr3[] = {0x05, 0x3A, 0x3A, 0x05, 0x3A, 0x3A};
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xB1, frmctr1, sizeof(frmctr1));
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xB2, frmctr2, sizeof(frmctr2));
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xB3, frmctr3, sizeof(frmctr3));

    /* Display inversion control */
    uint8_t invctr[] = {0x03};
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xB4, invctr, sizeof(invctr));

    /* Power control */
    uint8_t pwctr1[] = {0x62, 0x02, 0x04};
    uint8_t pwctr2[] = {0xC0};
    uint8_t pwctr3[] = {0x0D, 0x00};
    uint8_t pwctr4[] = {0x8D, 0x6A};
    uint8_t pwctr5[] = {0x8D, 0xEE};
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xC0, pwctr1, sizeof(pwctr1));
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xC1, pwctr2, sizeof(pwctr2));
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xC2, pwctr3, sizeof(pwctr3));
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xC3, pwctr4, sizeof(pwctr4));
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xC4, pwctr5, sizeof(pwctr5));

    /* VCOM control */
    uint8_t vmctr1[] = {0x0E};
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xC5, vmctr1, sizeof(vmctr1));

    /* Positive gamma */
    uint8_t gmctrp1[] = {0x10, 0x0E, 0x02, 0x03, 0x0E, 0x07, 0x02, 0x07,
                         0x0A, 0x12, 0x27, 0x37, 0x00, 0x0D, 0x0E, 0x10};
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xE0, gmctrp1, sizeof(gmctrp1));

    /* Negative gamma */
    uint8_t gmctrn1[] = {0x10, 0x0E, 0x03, 0x03, 0x0F, 0x06, 0x02, 0x08,
                         0x0A, 0x13, 0x26, 0x36, 0x00, 0x0D, 0x0E, 0x10};
    esp_lcd_panel_io_tx_param(bb_display_st77xx_panel_io, 0xE1, gmctrn1, sizeof(gmctrn1));

    return ESP_OK;
}

static bb_err_t st7735_init(uint16_t *width_out, uint16_t *height_out)
{
    bb_err_t err = BB_OK;

    /* Configure backlight GPIO (active-low: 1 = off during init). */
    gpio_set_direction(PIN_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_BL, 1);

    /* Initialize SPI bus and panel I/O. */
    err = bb_display_st77xx_init_bus();
    if (err != BB_OK) {
        bb_log_e(TAG, "init_bus failed");
        return err;
    }

    /* Create panel handle using ESP-IDF ST7789 driver (which works for ST7735). */
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

#if !CONFIG_BB_DISPLAY_ST7735_SKIP_VENDOR_INIT
    /* Apply ST7735-specific vendor init (gamma, power, frame rate).
     * Required by the LilyGo T-Dongle S3 panel; consumers with a
     * panel that the stock driver programs correctly can opt out
     * via CONFIG_BB_DISPLAY_ST7735_SKIP_VENDOR_INIT. */
    st7735_vendor_init();
#endif

    /* Panel config. */
    esp_lcd_panel_invert_color(bb_display_st77xx_panel, true);
    esp_lcd_panel_set_gap(bb_display_st77xx_panel, LCD_OFFSET_X, LCD_OFFSET_Y);
    esp_lcd_panel_swap_xy(bb_display_st77xx_panel, true);
    esp_lcd_panel_mirror(bb_display_st77xx_panel, false, true);
    esp_lcd_panel_disp_on_off(bb_display_st77xx_panel, true);

    /* Turn on backlight (active-low: 0 = on). */
    gpio_set_level(PIN_LCD_BL, 0);

    bb_log_i(TAG, "st7735 ready");
    *width_out = LCD_WIDTH;
    *height_out = LCD_HEIGHT;
    return BB_OK;
}

static bb_err_t st7735_set_rotation(uint16_t deg, uint16_t *w, uint16_t *h)
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
    .name         = "st7735",
    .probe        = NULL,
    .init         = st7735_init,
    .clear        = bb_display_st77xx_clear,
    .blit         = bb_display_st77xx_blit,
    .flush        = NULL,
    .off          = bb_display_st77xx_off,
    .draw_text    = NULL,
    .set_rotation = st7735_set_rotation,
};

void bb_display_register__st7735(void) __attribute__((constructor));
void bb_display_register__st7735(void)
{
    bb_display_register_backend(&s_backend);
}
