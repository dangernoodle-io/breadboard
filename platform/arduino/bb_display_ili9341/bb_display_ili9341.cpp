#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>
#include <string.h>
#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_display_ili9341.h"
#include "bb_log.h"

static const char *TAG = "bb_display_ili9341";

/* Pin defines come from consumer board header via -include. */
#ifndef PIN_LCD_CS
#error "PIN_LCD_CS undefined — add it to your board header"
#endif

#define ILI9341_NATIVE_W 240
#define ILI9341_NATIVE_H 320

static SPIClass *s_spi = nullptr;
static SPISettings s_spi_settings(40000000, MSBFIRST, SPI_MODE0);

static inline void cs_low()   { digitalWrite(PIN_LCD_CS, LOW); }
static inline void cs_high()  { digitalWrite(PIN_LCD_CS, HIGH); }
static inline void dc_cmd()   { digitalWrite(PIN_LCD_DC, LOW); }
static inline void dc_data()  { digitalWrite(PIN_LCD_DC, HIGH); }

static void send_cmd(uint8_t cmd) {
    dc_cmd();
    cs_low();
    s_spi->transfer(cmd);
    cs_high();
}

static void send_data(const uint8_t *data, size_t n) {
    dc_data();
    cs_low();
    for (size_t i = 0; i < n; i++) s_spi->transfer(data[i]);
    cs_high();
}

static void send_data_byte(uint8_t b) {
    uint8_t tmp = b;
    send_data(&tmp, 1);
}

static void send_pixels(const uint16_t *pix, size_t n) {
    /* ILI9341 expects big-endian on wire. */
    dc_data();
    cs_low();
    for (size_t i = 0; i < n; i++) {
        uint16_t c = pix[i];
        s_spi->transfer((uint8_t)(c >> 8));
        s_spi->transfer((uint8_t)(c & 0xFF));
    }
    cs_high();
}

/* Read 4 bytes after sending RDDID (0xD3). */
static void rddid4(uint8_t out[4]) {
    s_spi->beginTransaction(s_spi_settings);
    dc_cmd();
    cs_low();
    s_spi->transfer(0xD3);
    cs_high();
    dc_data();
    cs_low();
    for (int i = 0; i < 4; i++) out[i] = s_spi->transfer(0x00);
    cs_high();
    s_spi->endTransaction();
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
        digitalWrite(PIN_LCD_RST, HIGH);
        delay(5);
        digitalWrite(PIN_LCD_RST, LOW);
        delay(20);
        digitalWrite(PIN_LCD_RST, HIGH);
        delay(150);
    }
#endif
#ifdef PIN_LCD_BL
    if (PIN_LCD_BL >= 0) {
        pinMode(PIN_LCD_BL, OUTPUT);
        digitalWrite(PIN_LCD_BL, HIGH);  /* assume active-high */
    }
#endif
    s_spi = &SPI;
    s_spi->begin();
    s_pins_inited = true;
}

static bb_err_t ili9341_probe(void) {
    init_pins_once();
    uint8_t id[4];
    rddid4(id);
    /* Same id-layout tolerance as ESP-IDF backend. */
    bool match = (id[1] == 0x00 && id[2] == 0x93 && id[3] == 0x41) ||
                 (id[0] == 0x00 && id[1] == 0x93 && id[2] == 0x41) ||
                 (id[2] == 0x93 && id[3] == 0x41);
    if (!match) {
        bb_log_w(TAG, "RDDID mismatch: %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
        return BB_ERR_NOT_FOUND;
    }
    bb_log_i(TAG, "RDDID match: %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
    return BB_OK;
}

static bb_err_t ili9341_init(uint16_t *w, uint16_t *h) {
    init_pins_once();
    s_spi->beginTransaction(s_spi_settings);
    /* Software reset and basic init. */
    send_cmd(0x01);  delay(150);                         /* SWRESET */
    send_cmd(0x28);                                       /* DISPOFF */
    send_cmd(0x3A); send_data_byte(0x55);                 /* COLMOD: 16bpp */
    send_cmd(0x36); send_data_byte(0x28);                 /* MADCTL: BGR + landscape */
    send_cmd(0x11);  delay(120);                          /* SLPOUT */
    send_cmd(0x29);  delay(20);                           /* DISPON */
    s_spi->endTransaction();

    *w = ILI9341_NATIVE_H;  /* landscape */
    *h = ILI9341_NATIVE_W;
    bb_log_i(TAG, "ready %ux%u", (unsigned)*w, (unsigned)*h);
    return BB_OK;
}

static void set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t x1 = x + w - 1, y1 = y + h - 1;
    s_spi->beginTransaction(s_spi_settings);
    send_cmd(0x2A);
    uint8_t cax[4] = { (uint8_t)(x>>8), (uint8_t)(x&0xFF), (uint8_t)(x1>>8), (uint8_t)(x1&0xFF) };
    send_data(cax, 4);
    send_cmd(0x2B);
    uint8_t pax[4] = { (uint8_t)(y>>8), (uint8_t)(y&0xFF), (uint8_t)(y1>>8), (uint8_t)(y1&0xFF) };
    send_data(pax, 4);
    send_cmd(0x2C);  /* RAMWR */
    s_spi->endTransaction();
}

static void ili9341_clear(uint16_t rgb565) {
    set_addr_window(0, 0, ILI9341_NATIVE_H, ILI9341_NATIVE_W);
    s_spi->beginTransaction(s_spi_settings);
    dc_data();
    cs_low();
    /* 320x240 = 76800 pixels — small loop, no buffer needed. */
    uint8_t hi = rgb565 >> 8, lo = rgb565 & 0xFF;
    for (uint32_t i = 0; i < (uint32_t)ILI9341_NATIVE_W * ILI9341_NATIVE_H; i++) {
        s_spi->transfer(hi);
        s_spi->transfer(lo);
    }
    cs_high();
    s_spi->endTransaction();
}

static void ili9341_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels) {
    if (!pixels || !w || !h) return;
    set_addr_window((uint16_t)x, (uint16_t)y, w, h);
    s_spi->beginTransaction(s_spi_settings);
    send_pixels(pixels, (size_t)w * h);
    s_spi->endTransaction();
}

static void ili9341_off(void) {
    s_spi->beginTransaction(s_spi_settings);
    send_cmd(0x28);  /* DISPOFF */
    send_cmd(0x10);  /* SLPIN */
    s_spi->endTransaction();
#ifdef PIN_LCD_BL
    if (PIN_LCD_BL >= 0) digitalWrite(PIN_LCD_BL, LOW);
#endif
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

extern "C" {
void bb_display_register__ili9341(void) __attribute__((constructor));
void bb_display_register__ili9341(void) {
    bb_display_register_backend(&s_backend);
}
}  /* extern "C" */
