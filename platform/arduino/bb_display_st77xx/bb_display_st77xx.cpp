extern "C" {
#include "bb_display.h"
#include "bb_display_backend.h"
}
#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>
#include <string.h>

#ifndef PIN_LCD_CS
#error "PIN_LCD_CS undefined — add it to your board header"
#endif

/* ST77xx panels are typically 80x160 (st7735 small variant) or
 * 240x240 / 240x320 (st7789). Override via build_flags if needed. */
#ifndef BB_DISPLAY_ST77XX_WIDTH
#define BB_DISPLAY_ST77XX_WIDTH 160
#endif
#ifndef BB_DISPLAY_ST77XX_HEIGHT
#define BB_DISPLAY_ST77XX_HEIGHT 80
#endif
#ifndef BB_DISPLAY_ST77XX_OFFSET_X
#define BB_DISPLAY_ST77XX_OFFSET_X 0
#endif
#ifndef BB_DISPLAY_ST77XX_OFFSET_Y
#define BB_DISPLAY_ST77XX_OFFSET_Y 0
#endif

#ifndef BB_DISPLAY_ST77XX_PIXEL_CLK_HZ
#define BB_DISPLAY_ST77XX_PIXEL_CLK_HZ 20000000
#endif

static SPIClass *s_spi = nullptr;
static SPISettings s_spi_settings(BB_DISPLAY_ST77XX_PIXEL_CLK_HZ, MSBFIRST, SPI_MODE0);

static inline void cs_low()  { digitalWrite(PIN_LCD_CS, LOW); }
static inline void cs_high() { digitalWrite(PIN_LCD_CS, HIGH); }
static inline void dc_cmd()  { digitalWrite(PIN_LCD_DC, LOW); }
static inline void dc_data() { digitalWrite(PIN_LCD_DC, HIGH); }

static void send_cmd(uint8_t cmd) {
    dc_cmd();  cs_low();  s_spi->transfer(cmd);  cs_high();
}

static void send_data(const uint8_t *data, size_t n) {
    dc_data();  cs_low();
    for (size_t i = 0; i < n; i++) s_spi->transfer(data[i]);
    cs_high();
}

static void send_data_byte(uint8_t b) { send_data(&b, 1); }

static void send_pixels(const uint16_t *pix, size_t n) {
    /* ST77xx expects big-endian RGB565 on the wire. */
    dc_data();  cs_low();
    for (size_t i = 0; i < n; i++) {
        uint16_t c = pix[i];
        s_spi->transfer((uint8_t)(c >> 8));
        s_spi->transfer((uint8_t)(c & 0xFF));
    }
    cs_high();
}

static bool s_pins_inited = false;

static void init_pins_once(void) {
    if (s_pins_inited) return;
    pinMode(PIN_LCD_CS, OUTPUT);
    pinMode(PIN_LCD_DC, OUTPUT);
    digitalWrite(PIN_LCD_CS, HIGH);
    digitalWrite(PIN_LCD_DC, HIGH);
#ifdef PIN_LCD_RST
    if (PIN_LCD_RST >= 0) {
        pinMode(PIN_LCD_RST, OUTPUT);
        digitalWrite(PIN_LCD_RST, HIGH); delay(5);
        digitalWrite(PIN_LCD_RST, LOW);  delay(20);
        digitalWrite(PIN_LCD_RST, HIGH); delay(150);
    }
#endif
#ifdef PIN_LCD_BL
    if (PIN_LCD_BL >= 0) {
        pinMode(PIN_LCD_BL, OUTPUT);
        /* Active-low on the LilyGo dongle; some boards are active-high.
         * Override with -DBB_DISPLAY_ST77XX_BL_ACTIVE_HIGH if needed. */
#ifdef BB_DISPLAY_ST77XX_BL_ACTIVE_HIGH
        digitalWrite(PIN_LCD_BL, HIGH);
#else
        digitalWrite(PIN_LCD_BL, LOW);
#endif
    }
#endif
    s_spi = &SPI;
    s_spi->begin();
    s_pins_inited = true;
}

/* ST7735 vendor-init overlay (gamma/power/frame). Lifted from
 * platform/espidf/bb_display_st77xx/bb_display_st7735.c lines 20-67. */
static void st7735_vendor_init(void) {
    uint8_t frmctr1[] = {0x05, 0x3A, 0x3A};
    uint8_t frmctr2[] = {0x05, 0x3A, 0x3A};
    uint8_t frmctr3[] = {0x05, 0x3A, 0x3A, 0x05, 0x3A, 0x3A};
    send_cmd(0xB1); send_data(frmctr1, sizeof(frmctr1));
    send_cmd(0xB2); send_data(frmctr2, sizeof(frmctr2));
    send_cmd(0xB3); send_data(frmctr3, sizeof(frmctr3));
    uint8_t invctr[] = {0x03};
    send_cmd(0xB4); send_data(invctr, sizeof(invctr));
    uint8_t pwctr1[] = {0x62, 0x02, 0x04};
    uint8_t pwctr2[] = {0xC0};
    uint8_t pwctr3[] = {0x0D, 0x00};
    uint8_t pwctr4[] = {0x8D, 0x6A};
    uint8_t pwctr5[] = {0x8D, 0xEE};
    send_cmd(0xC0); send_data(pwctr1, sizeof(pwctr1));
    send_cmd(0xC1); send_data(pwctr2, sizeof(pwctr2));
    send_cmd(0xC2); send_data(pwctr3, sizeof(pwctr3));
    send_cmd(0xC3); send_data(pwctr4, sizeof(pwctr4));
    send_cmd(0xC4); send_data(pwctr5, sizeof(pwctr5));
    uint8_t vmctr1[] = {0x0E};
    send_cmd(0xC5); send_data(vmctr1, sizeof(vmctr1));
    uint8_t gmctrp1[] = {0x10, 0x0E, 0x02, 0x03, 0x0E, 0x07, 0x02, 0x07,
                         0x0A, 0x12, 0x27, 0x37, 0x00, 0x0D, 0x0E, 0x10};
    send_cmd(0xE0); send_data(gmctrp1, sizeof(gmctrp1));
    uint8_t gmctrn1[] = {0x10, 0x0E, 0x03, 0x03, 0x0F, 0x06, 0x02, 0x08,
                         0x0A, 0x13, 0x26, 0x36, 0x00, 0x0D, 0x0E, 0x10};
    send_cmd(0xE1); send_data(gmctrn1, sizeof(gmctrn1));
}

/* Common init for both variants — software reset, COLMOD, MADCTL, sleep
 * out, display on. Caller adds vendor overlay between SLPOUT and DISPON
 * for st7735. */
static void common_init_pre(void) {
    s_spi->beginTransaction(s_spi_settings);
    send_cmd(0x01);  delay(150);                  /* SWRESET */
    send_cmd(0x11);  delay(120);                  /* SLPOUT */
    send_cmd(0x3A);  send_data_byte(0x05);        /* COLMOD: 16bpp */
    send_cmd(0x36);  send_data_byte(0x60);        /* MADCTL: BGR + MX swap (landscape) */
}
static void common_init_post(void) {
    /* Inversion on for typical IPS modules; some panels need it off. */
    send_cmd(0x21);                               /* INVON */
    send_cmd(0x29);  delay(20);                   /* DISPON */
    s_spi->endTransaction();
}

static bb_err_t st7735_init(uint16_t *w, uint16_t *h) {
    init_pins_once();
    common_init_pre();
    st7735_vendor_init();
    common_init_post();
    *w = BB_DISPLAY_ST77XX_WIDTH;
    *h = BB_DISPLAY_ST77XX_HEIGHT;
    return BB_OK;
}

static bb_err_t st7789_init(uint16_t *w, uint16_t *h) {
    init_pins_once();
    common_init_pre();
    common_init_post();
    *w = BB_DISPLAY_ST77XX_WIDTH;
    *h = BB_DISPLAY_ST77XX_HEIGHT;
    return BB_OK;
}

static void set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t x0 = x + BB_DISPLAY_ST77XX_OFFSET_X;
    uint16_t y0 = y + BB_DISPLAY_ST77XX_OFFSET_Y;
    uint16_t x1 = x0 + w - 1, y1 = y0 + h - 1;
    s_spi->beginTransaction(s_spi_settings);
    send_cmd(0x2A);
    uint8_t cax[4] = { (uint8_t)(x0>>8), (uint8_t)(x0&0xFF), (uint8_t)(x1>>8), (uint8_t)(x1&0xFF) };
    send_data(cax, 4);
    send_cmd(0x2B);
    uint8_t pax[4] = { (uint8_t)(y0>>8), (uint8_t)(y0&0xFF), (uint8_t)(y1>>8), (uint8_t)(y1&0xFF) };
    send_data(pax, 4);
    send_cmd(0x2C);  /* RAMWR */
    s_spi->endTransaction();
}

static void common_clear(uint16_t rgb565) {
    set_addr_window(0, 0, BB_DISPLAY_ST77XX_WIDTH, BB_DISPLAY_ST77XX_HEIGHT);
    s_spi->beginTransaction(s_spi_settings);
    dc_data(); cs_low();
    uint8_t hi = rgb565 >> 8, lo = rgb565 & 0xFF;
    for (uint32_t i = 0; i < (uint32_t)BB_DISPLAY_ST77XX_WIDTH * BB_DISPLAY_ST77XX_HEIGHT; i++) {
        s_spi->transfer(hi);
        s_spi->transfer(lo);
    }
    cs_high();
    s_spi->endTransaction();
}

static void common_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels) {
    if (!pixels || !w || !h) return;
    set_addr_window((uint16_t)x, (uint16_t)y, w, h);
    s_spi->beginTransaction(s_spi_settings);
    send_pixels(pixels, (size_t)w * h);
    s_spi->endTransaction();
}

static void common_off(void) {
    s_spi->beginTransaction(s_spi_settings);
    send_cmd(0x28);  /* DISPOFF */
    send_cmd(0x10);  /* SLPIN  */
    s_spi->endTransaction();
#ifdef PIN_LCD_BL
    if (PIN_LCD_BL >= 0) {
#ifdef BB_DISPLAY_ST77XX_BL_ACTIVE_HIGH
        digitalWrite(PIN_LCD_BL, LOW);
#else
        digitalWrite(PIN_LCD_BL, HIGH);
#endif
    }
#endif
}

static bb_err_t st77xx_set_rotation(uint16_t deg, uint16_t *w, uint16_t *h) {
    /* MADCTL register (0x36) bits:
     * MX=0x40, MY=0x80, MV=0x20, BGR=0x08 */
    uint8_t madctl = 0x08;  /* BGR always on */
    uint16_t width = BB_DISPLAY_ST77XX_WIDTH, height = BB_DISPLAY_ST77XX_HEIGHT;

    switch (deg) {
        case 0:
            madctl |= 0x00;  /* no flips */
            break;
        case 90:
            madctl |= (0x20 | 0x40);  /* MV | MX (swap + mirror_x) */
            width = BB_DISPLAY_ST77XX_HEIGHT;
            height = BB_DISPLAY_ST77XX_WIDTH;
            break;
        case 180:
            madctl |= (0x80 | 0x40);  /* MY | MX (mirror_y + mirror_x) */
            break;
        case 270:
            madctl |= (0x20 | 0x80);  /* MV | MY (swap + mirror_y) */
            width = BB_DISPLAY_ST77XX_HEIGHT;
            height = BB_DISPLAY_ST77XX_WIDTH;
            break;
        default:
            return BB_ERR_INVALID_ARG;
    }

    s_spi->beginTransaction(s_spi_settings);
    send_cmd(0x36);
    send_data_byte(madctl);
    s_spi->endTransaction();

    *w = width;
    *h = height;
    return BB_OK;
}

extern "C" {

#if defined(BB_DISPLAY_ST77XX_VARIANT_ST7735) || \
    (!defined(BB_DISPLAY_ST77XX_VARIANT_ST7789) && !defined(BB_DISPLAY_ST77XX_REGISTER_BOTH))
static const bb_display_backend_t s_backend_st7735 = {
    .name         = "st7735",
    .probe        = NULL,
    .init         = st7735_init,
    .clear        = common_clear,
    .blit         = common_blit,
    .flush        = NULL,
    .off          = common_off,
    .draw_text    = NULL,
    .set_rotation = st77xx_set_rotation,
};
void bb_display_register__st7735(void) __attribute__((constructor));
void bb_display_register__st7735(void) {
    bb_display_register_backend(&s_backend_st7735);
}
#endif

#if defined(BB_DISPLAY_ST77XX_VARIANT_ST7789) || defined(BB_DISPLAY_ST77XX_REGISTER_BOTH)
static const bb_display_backend_t s_backend_st7789 = {
    .name         = "st7789",
    .probe        = NULL,
    .init         = st7789_init,
    .clear        = common_clear,
    .blit         = common_blit,
    .flush        = NULL,
    .off          = common_off,
    .draw_text    = NULL,
    .set_rotation = st77xx_set_rotation,
};
void bb_display_register__st7789(void) __attribute__((constructor));
void bb_display_register__st7789(void) {
    bb_display_register_backend(&s_backend_st7789);
}
#endif

}  /* extern "C" */
