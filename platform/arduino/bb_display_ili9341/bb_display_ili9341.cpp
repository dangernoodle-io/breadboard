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
/* Reads max ~10 MHz per ILI9341 datasheet; writes can run at full speed. */
static SPISettings s_spi_read_settings(5000000, MSBFIRST, SPI_MODE0);

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

/* RDDID4 (0xD3): single CS-low transaction — cmd byte with DC=0, then 4
 * read bytes with DC=1. Must NOT raise CS between cmd and read or the
 * panel resets its read pointer. Slower clock for reads. */
static void rddid4(uint8_t out[4]) {
    s_spi->beginTransaction(s_spi_read_settings);
    cs_low();
    dc_cmd();
    s_spi->transfer(0xD3);
    dc_data();
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
#ifdef BB_DISPLAY_SHIELD_IDLE_CS_PINS
    /* Hold sibling CS pins (touch, SD, etc. on shared SPI shields) HIGH
     * so they don't contend MISO during our reads. */
    {
        static const int8_t s_idle[] = { BB_DISPLAY_SHIELD_IDLE_CS_PINS };
        for (size_t i = 0; i < sizeof(s_idle); i++) {
            if (s_idle[i] < 0) continue;
            pinMode(s_idle[i], OUTPUT);
            digitalWrite(s_idle[i], HIGH);
        }
    }
#endif
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
#ifdef BB_DISPLAY_ILI9341_TRUST
        /* Board declares the panel is ILI9341 but MISO isn't wired through
         * (common on cheap shields where SDO is left floating). Trust the
         * board header and proceed with init. */
        bb_log_w(TAG, "RDDID mismatch: %02x %02x %02x %02x (TRUST set, proceeding)",
                 id[0], id[1], id[2], id[3]);
        return BB_OK;
#else
        bb_log_w(TAG, "RDDID mismatch: %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
        return BB_ERR_NOT_FOUND;
#endif
    }
    bb_log_i(TAG, "RDDID match: %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
    return BB_OK;
}

/* Init sequence from Adafruit_ILI9341 (BSD) — proven on real silicon.
 * Format: cmd, n_args, args... ; n_args MSB=1 means delay(150) after. */
static const uint8_t ili9341_init_cmds[] = {
    0xEF, 3, 0x03, 0x80, 0x02,
    0xCF, 3, 0x00, 0xC1, 0x30,
    0xED, 4, 0x64, 0x03, 0x12, 0x81,
    0xE8, 3, 0x85, 0x00, 0x78,
    0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
    0xF7, 1, 0x20,
    0xEA, 2, 0x00, 0x00,
    0xC0, 1, 0x23,                  /* PWCTR1 */
    0xC1, 1, 0x10,                  /* PWCTR2 */
    0xC5, 2, 0x3E, 0x28,            /* VMCTR1 */
    0xC7, 1, 0x86,                  /* VMCTR2 */
    0x36, 1, 0x28,                  /* MADCTL: MV|BGR (landscape) */
    0x37, 1, 0x00,                  /* VSCRSADD */
    0x3A, 1, 0x55,                  /* PIXFMT: 16bpp */
    0xB1, 2, 0x00, 0x18,            /* FRMCTR1 */
    0xB6, 3, 0x08, 0x82, 0x27,      /* DFUNCTR */
    0xF2, 1, 0x00,                  /* 3Gamma off */
    0x26, 1, 0x01,                  /* GAMMASET */
    0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08,
              0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
    0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07,
              0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
    0x11, 0x80,                     /* SLPOUT + delay */
    0x29, 0x80,                     /* DISPON + delay */
    0x00
};

static bb_err_t ili9341_init(uint16_t *w, uint16_t *h) {
    init_pins_once();
    s_spi->beginTransaction(s_spi_settings);
    send_cmd(0x01); delay(150);  /* SWRESET */
    const uint8_t *p = ili9341_init_cmds;
    while (*p) {
        uint8_t cmd = *p++;
        uint8_t x   = *p++;
        uint8_t n   = x & 0x7F;
        send_cmd(cmd);
        if (n) send_data(p, n);
        p += n;
        if (x & 0x80) delay(150);
    }
    s_spi->endTransaction();

    *w = ILI9341_NATIVE_H;  /* landscape */
    *h = ILI9341_NATIVE_W;
    bb_log_i(TAG, "ready %ux%u", (unsigned)*w, (unsigned)*h);
    return BB_OK;
}

/* CS stays LOW for the entire CASET+PASET+RAMWR+pixel-data batch; raising
 * CS between RAMWR and the pixel stream cancels the write on many ILI9341
 * implementations. */
static inline void caset_paset_ramwr(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t x1 = x + w - 1, y1 = y + h - 1;
    dc_cmd();  s_spi->transfer(0x2A);
    dc_data(); s_spi->transfer(x>>8); s_spi->transfer(x&0xFF);
               s_spi->transfer(x1>>8); s_spi->transfer(x1&0xFF);
    dc_cmd();  s_spi->transfer(0x2B);
    dc_data(); s_spi->transfer(y>>8); s_spi->transfer(y&0xFF);
               s_spi->transfer(y1>>8); s_spi->transfer(y1&0xFF);
    dc_cmd();  s_spi->transfer(0x2C);  /* RAMWR */
    dc_data();
}

static void ili9341_clear(uint16_t rgb565) {
    s_spi->beginTransaction(s_spi_settings);
    cs_low();
    caset_paset_ramwr(0, 0, ILI9341_NATIVE_H, ILI9341_NATIVE_W);
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
    s_spi->beginTransaction(s_spi_settings);
    cs_low();
    caset_paset_ramwr((uint16_t)x, (uint16_t)y, w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        s_spi->transfer(pixels[i] >> 8);
        s_spi->transfer(pixels[i] & 0xFF);
    }
    cs_high();
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

static bb_err_t ili9341_set_rotation(uint16_t deg, uint16_t *w, uint16_t *h) {
    /* MADCTL register (0x36) bits:
     * MX=0x40, MY=0x80, MV=0x20, BGR=0x08 */
    uint8_t madctl = 0x08;  /* BGR always on */
    uint16_t width = ILI9341_NATIVE_W, height = ILI9341_NATIVE_H;

    switch (deg) {
        case 0:
            madctl |= 0x40;  /* MX */
            break;
        case 90:
            madctl |= (0x20 | 0x08);  /* MV | BGR (landscape) */
            /* 90° rotation swaps dimensions */
            width = ILI9341_NATIVE_H;
            height = ILI9341_NATIVE_W;
            break;
        case 180:
            madctl |= (0x80 | 0x40);  /* MY | MX */
            break;
        case 270:
            madctl |= (0x20 | 0x80 | 0x40);  /* MV | MY | MX */
            /* 270° rotation swaps dimensions */
            width = ILI9341_NATIVE_H;
            height = ILI9341_NATIVE_W;
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

static const bb_display_backend_t s_backend = {
    .name         = "ili9341",
    .probe        = ili9341_probe,
    .init         = ili9341_init,
    .clear        = ili9341_clear,
    .blit         = ili9341_blit,
    .flush        = NULL,
    .off          = ili9341_off,
    .draw_text    = NULL,
    .set_rotation = ili9341_set_rotation,
};

extern "C" {
void bb_display_register__ili9341(void) __attribute__((constructor));
void bb_display_register__ili9341(void) {
    bb_display_register_backend(&s_backend);
}
}  /* extern "C" */
