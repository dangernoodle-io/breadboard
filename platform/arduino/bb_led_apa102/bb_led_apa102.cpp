#include <Arduino.h>

extern "C" {
#include "bb_led_apa102.h"
#include "bb_led_driver.h"
#include "bb_led_gamma.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    int pin_clk;
    int pin_din;
    uint16_t led_count;
    uint8_t *rgb;             // 3 * led_count bytes (base color)
    uint8_t *bri;             // 1 * led_count bytes (per-LED brightness 0..31)
    bool *enabled;            // 1 * led_count bytes
    uint16_t *level16;        // per-LED fine brightness (set via set_level)
    bool *level_set;          // per-LED: use the level16 path instead of bri
    bb_led_driver_t *drv;     // self-ref for cleanup
} state_t;

static void tx_byte(int clk, int din, uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        digitalWrite(din, (b >> i) & 1);
        digitalWrite(clk, 1);
        digitalWrite(clk, 0);
    }
}

static bb_err_t do_flush(state_t *s) {
    // Start frame: 4 zero bytes.
    for (int i = 0; i < 4; i++) tx_byte(s->pin_clk, s->pin_din, 0);

    // Per-LED frames.
    for (uint16_t i = 0; i < s->led_count; i++) {
        if (!s->enabled[i]) {
            tx_byte(s->pin_clk, s->pin_din, 0xE0);
            tx_byte(s->pin_clk, s->pin_din, 0);
            tx_byte(s->pin_clk, s->pin_din, 0);
            tx_byte(s->pin_clk, s->pin_din, 0);
            continue;
        }
        uint8_t r = s->rgb[i*3 + 0], g = s->rgb[i*3 + 1], b = s->rgb[i*3 + 2];
        uint8_t bri = s->bri[i];
        if (s->level_set[i]) {
            uint32_t env = bb_led_gamma_cie(s->level16[i]);  // 0..65535
            r = (uint8_t)((uint32_t)r * env / 65535u);
            g = (uint8_t)((uint32_t)g * env / 65535u);
            b = (uint8_t)((uint32_t)b * env / 65535u);
            bri = 31;
        }
        tx_byte(s->pin_clk, s->pin_din, 0xE0 | (bri & 0x1F));
        tx_byte(s->pin_clk, s->pin_din, b);  // B
        tx_byte(s->pin_clk, s->pin_din, g);  // G
        tx_byte(s->pin_clk, s->pin_din, r);  // R
    }

    // End frame: (N+15)/16 bytes of 0xFF.
    uint16_t end_bytes = (s->led_count + 15) / 16;
    if (end_bytes < 1) end_bytes = 1;
    for (uint16_t i = 0; i < end_bytes; i++) tx_byte(s->pin_clk, s->pin_din, 0xFF);

    return BB_OK;
}

static bb_err_t op_set_on(void *st, uint16_t idx, bool on) {
    state_t *s = (state_t *)st;
    s->enabled[idx] = on;
    return BB_OK;
}

static bb_err_t op_set_brightness(void *st, uint16_t idx, uint8_t pct) {
    state_t *s = (state_t *)st;
    s->bri[idx] = (uint8_t)(((uint32_t)pct * 31) / 100);
    s->level_set[idx] = false;  // revert to the coarse 5-bit global path
    return BB_OK;
}

static bb_err_t op_set_level(void *st, uint16_t idx, uint16_t level) {
    state_t *s = (state_t *)st;
    s->level16[idx] = level;
    s->level_set[idx] = true;   // scale base color by gamma(level) at flush
    return BB_OK;
}

static bb_err_t op_set_color(void *st, uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    state_t *s = (state_t *)st;
    s->rgb[idx*3 + 0] = r;
    s->rgb[idx*3 + 1] = g;
    s->rgb[idx*3 + 2] = b;
    return BB_OK;
}

static bb_err_t op_flush(void *st) {
    return do_flush((state_t *)st);
}

static bb_err_t op_close(void *st) {
    state_t *s = (state_t *)st;
    digitalWrite(s->pin_clk, 0);
    digitalWrite(s->pin_din, 0);
    free(s->rgb);
    free(s->bri);
    free(s->enabled);
    free(s->level16);
    free(s->level_set);
    free(s->drv);
    free(s);
    return BB_OK;
}

bb_err_t bb_led_apa102_open(const bb_led_apa102_cfg_t *cfg, bb_led_handle_t *out) {
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->pin_clk < 0 || cfg->pin_din < 0) return BB_ERR_INVALID_ARG;
    if (cfg->led_count == 0) return BB_ERR_INVALID_ARG;
    if (cfg->global_brightness_31 > 31) return BB_ERR_INVALID_ARG;

    pinMode(cfg->pin_clk, OUTPUT);
    pinMode(cfg->pin_din, OUTPUT);
    digitalWrite(cfg->pin_clk, 0);
    digitalWrite(cfg->pin_din, 0);

    state_t *s = (state_t *)calloc(1, sizeof *s);
    if (!s) return BB_ERR_NO_SPACE;
    s->pin_clk = cfg->pin_clk;
    s->pin_din = cfg->pin_din;
    s->led_count = cfg->led_count;
    s->rgb = (uint8_t *)calloc(cfg->led_count * 3, 1);
    s->bri = (uint8_t *)calloc(cfg->led_count, 1);
    s->enabled = (bool *)calloc(cfg->led_count, sizeof(bool));
    s->level16 = (uint16_t *)calloc(cfg->led_count, sizeof(uint16_t));
    s->level_set = (bool *)calloc(cfg->led_count, sizeof(bool));
    if (!s->rgb || !s->bri || !s->enabled || !s->level16 || !s->level_set) {
        free(s->rgb); free(s->bri); free(s->enabled);
        free(s->level16); free(s->level_set); free(s);
        return BB_ERR_NO_SPACE;
    }

    // Initialize per-LED brightness to global default.
    for (uint16_t i = 0; i < cfg->led_count; i++) s->bri[i] = cfg->global_brightness_31;

    bb_led_driver_t *drv = (bb_led_driver_t *)calloc(1, sizeof *drv);
    if (!drv) { free(s->rgb); free(s->bri); free(s->enabled); free(s->level16); free(s->level_set); free(s); return BB_ERR_NO_SPACE; }
    *drv = (bb_led_driver_t){
        .set_on = op_set_on,
        .set_brightness = op_set_brightness,
        .set_level = op_set_level,
        .set_color = op_set_color,
        .flush = op_flush,
        .close = op_close,
        .caps = (bb_led_caps_t)(BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB),
        .count = cfg->led_count,
        .name = "apa102",
    };
    s->drv = drv;

    do_flush(s);  // initial dark state to strip.

    bb_err_t rc = bb_led_handle_create(drv, s, out);
    if (rc != BB_OK) { free(s->rgb); free(s->bri); free(s->enabled); free(s->level16); free(s->level_set); free(drv); free(s); }
    return rc;
}

}
