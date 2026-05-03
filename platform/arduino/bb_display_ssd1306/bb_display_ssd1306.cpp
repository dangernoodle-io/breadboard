extern "C" {
#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_display_ssd1306.h"
}
#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>
#include <string.h>

/* Tunables — ESP-IDF backend has Kconfig knobs; Arduino has plain #defines.
 * Override via -DBB_DISPLAY_SSD1306_* in the consumer's build_flags. */
#ifndef BB_DISPLAY_SSD1306_I2C_ADDR
#define BB_DISPLAY_SSD1306_I2C_ADDR 0x3C
#endif
#ifndef BB_DISPLAY_SSD1306_HEIGHT
/* 128x64 is the dominant "0.96 inch" module size in the wild (and what most
 * SSD1315 modules ship with too). Override to 32 for the smaller 0.49"
 * 128x32 variant. */
#define BB_DISPLAY_SSD1306_HEIGHT 64
#endif
#define SSD1306_WIDTH  128
#define SSD1306_HEIGHT BB_DISPLAY_SSD1306_HEIGHT
#define SSD1306_PAGES  (SSD1306_HEIGHT / 8)

static uint8_t s_fb[SSD1306_WIDTH * SSD1306_PAGES];
static bool s_wire_inited = false;

static void wire_init_once() {
    if (s_wire_inited) return;
    Wire.begin();
    s_wire_inited = true;
}

/* Send a single command byte: control=0x00, then cmd. */
static void cmd(uint8_t c) {
    Wire.beginTransmission(BB_DISPLAY_SSD1306_I2C_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write(c);
    Wire.endTransmission();
}

/* Push the framebuffer over I²C in 16-byte data chunks. */
static void flush_fb(void) {
    /* Set page + column addressing range to whole-screen. */
    cmd(0x21); cmd(0); cmd(SSD1306_WIDTH - 1);
    cmd(0x22); cmd(0); cmd(SSD1306_PAGES - 1);

    const size_t chunk = 16;
    for (size_t i = 0; i < sizeof(s_fb); i += chunk) {
        Wire.beginTransmission(BB_DISPLAY_SSD1306_I2C_ADDR);
        Wire.write((uint8_t)0x40);  /* control: data follows */
        size_t n = sizeof(s_fb) - i < chunk ? sizeof(s_fb) - i : chunk;
        Wire.write(&s_fb[i], n);
        Wire.endTransmission();
    }
}

extern "C" {

static bb_err_t ssd1306_probe(void) {
    wire_init_once();
    Wire.beginTransmission(BB_DISPLAY_SSD1306_I2C_ADDR);
    /* endTransmission returns 0 on ACK, !=0 on NACK / timeout / bus error. */
    if (Wire.endTransmission() != 0) return BB_ERR_NOT_FOUND;
    return BB_OK;
}

static bb_err_t ssd1306_init(uint16_t *w, uint16_t *h) {
    wire_init_once();

    /* Standard SSD1306 init sequence — same as Adafruit's reference, just
     * inlined so we don't drag in their library. Differences for 32 vs 64
     * pixel height are handled below. */
    cmd(0xAE);              /* DISPLAYOFF */
    cmd(0xD5); cmd(0x80);   /* SETDISPLAYCLOCKDIV */
    cmd(0xA8); cmd(SSD1306_HEIGHT - 1);  /* SETMULTIPLEX */
    cmd(0xD3); cmd(0x00);   /* SETDISPLAYOFFSET */
    cmd(0x40);              /* SETSTARTLINE */
    cmd(0x8D); cmd(0x14);   /* CHARGEPUMP — internal */
    cmd(0x20); cmd(0x00);   /* MEMORYMODE — horizontal */
    cmd(0xA1);              /* SEGREMAP */
    cmd(0xC8);              /* COMSCANDEC */
#if SSD1306_HEIGHT == 32
    cmd(0xDA); cmd(0x02);   /* SETCOMPINS */
    cmd(0x81); cmd(0x8F);   /* SETCONTRAST */
#else
    cmd(0xDA); cmd(0x12);
    cmd(0x81); cmd(0xCF);
#endif
    cmd(0xD9); cmd(0xF1);   /* SETPRECHARGE */
    cmd(0xDB); cmd(0x40);   /* SETVCOMDETECT */
    cmd(0xA4);              /* DISPLAYALLON_RESUME */
    cmd(0xA6);              /* NORMALDISPLAY */
    cmd(0x2E);              /* DEACTIVATE_SCROLL */
    cmd(0xAF);              /* DISPLAYON */

    memset(s_fb, 0, sizeof(s_fb));
    flush_fb();

    *w = SSD1306_WIDTH;
    *h = SSD1306_HEIGHT;
    return BB_OK;
}

/* RGB565 → 1bpp via luma threshold. The previous "any non-zero = on" was too
 * aggressive: near-black backgrounds like 0x0004 mapped to ON and washed the
 * whole screen. Use a ~50% luma cutoff so callers can pass real RGB565 colors
 * intended for color panels and still get sensible mono rendering. */
static inline bool rgb565_to_mono(uint16_t c) {
    uint8_t r = (c >> 11) & 0x1F;       /* 0..31 */
    uint8_t g = (c >> 5)  & 0x3F;       /* 0..63 */
    uint8_t b = c & 0x1F;               /* 0..31 */
    /* Approx luma: r*2 + g + b*2, max = 31*2 + 63 + 31*2 = 187. Threshold 90 ≈ 48%. */
    return (r * 2 + g + b * 2) > 90;
}

static void set_pixel(int x, int y, bool on) {
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) return;
    uint8_t mask = (uint8_t)(1u << (y % 8));
    if (on) s_fb[(y / 8) * SSD1306_WIDTH + x] |=  mask;
    else    s_fb[(y / 8) * SSD1306_WIDTH + x] &= ~mask;
}

static void ssd1306_clear(uint16_t rgb565) {
    memset(s_fb, rgb565_to_mono(rgb565) ? 0xFF : 0x00, sizeof(s_fb));
    flush_fb();
}

static void ssd1306_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels) {
    if (!pixels || !w || !h) return;
    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            set_pixel(x + col, y + row, rgb565_to_mono(pixels[row * w + col]));
        }
    }
    flush_fb();
}

static void ssd1306_off(void) {
    cmd(0xAE);  /* DISPLAYOFF */
}

static const bb_display_backend_t s_backend = {
    .name      = "ssd1306",
    .probe     = ssd1306_probe,
    .init      = ssd1306_init,
    .clear     = ssd1306_clear,
    .blit      = ssd1306_blit,
    .flush     = NULL,
    .off       = ssd1306_off,
    .draw_text = NULL,
};

void bb_display_register__ssd1306(void) __attribute__((constructor));
void bb_display_register__ssd1306(void) {
    bb_display_register_backend(&s_backend);
}

}  /* extern "C" */
