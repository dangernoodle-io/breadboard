#include "bb_led_apa102.h"
#include "bb_led_driver.h"
#include "bb_led_apa102_host.h"
#include <stdlib.h>
#include <string.h>

#define BB_LED_APA102_HOST_MAX_SLOTS 4

typedef struct {
    uint8_t *buf;
    size_t len;
    bool active;
} slot_t;

static slot_t s_slots[BB_LED_APA102_HOST_MAX_SLOTS];

const uint8_t *bb_led_apa102_host_last_buf(int slot, size_t *out_len) {
    if (slot < 0 || slot >= BB_LED_APA102_HOST_MAX_SLOTS) return NULL;
    if (!s_slots[slot].active) return NULL;
    *out_len = s_slots[slot].len;
    return s_slots[slot].buf;
}

void bb_led_apa102_host_reset(void) {
    for (int i = 0; i < BB_LED_APA102_HOST_MAX_SLOTS; i++) {
        free(s_slots[i].buf);
        s_slots[i].buf = NULL;
        s_slots[i].len = 0;
        s_slots[i].active = false;
    }
}

typedef struct {
    uint16_t led_count;
    uint8_t *rgb;             // 3 * led_count bytes
    uint8_t *bri;             // 1 * led_count bytes (per-LED brightness 0..31)
    bool *enabled;            // 1 * led_count bytes
    bb_led_driver_t *drv;
    int slot;
} state_t;

// Append a byte to the slot buffer.
static void buf_append(slot_t *slot, uint8_t b) {
    uint8_t *new_buf = realloc(slot->buf, slot->len + 1);
    if (new_buf) {
        slot->buf = new_buf;
        slot->buf[slot->len] = b;
        slot->len++;
    }
}

static void tx_byte(slot_t *slot, uint8_t b) {
    buf_append(slot, b);
}

static bb_err_t do_flush(state_t *s) {
    slot_t *slot = &s_slots[s->slot];
    // Clear previous buffer.
    free(slot->buf);
    slot->buf = NULL;
    slot->len = 0;

    // Start frame: 4 zero bytes.
    for (int i = 0; i < 4; i++) tx_byte(slot, 0);

    // Per-LED frames.
    for (uint16_t i = 0; i < s->led_count; i++) {
        uint8_t bri = s->enabled[i] ? s->bri[i] : 0;
        tx_byte(slot, 0xE0 | (bri & 0x1F));
        if (s->enabled[i]) {
            tx_byte(slot, s->rgb[i*3 + 2]);  // B
            tx_byte(slot, s->rgb[i*3 + 1]);  // G
            tx_byte(slot, s->rgb[i*3 + 0]);  // R
        } else {
            tx_byte(slot, 0);
            tx_byte(slot, 0);
            tx_byte(slot, 0);
        }
    }

    // End frame: (N+15)/16 bytes of 0xFF.
    uint16_t end_bytes = (s->led_count + 15) / 16;
    if (end_bytes < 1) end_bytes = 1;
    for (uint16_t i = 0; i < end_bytes; i++) tx_byte(slot, 0xFF);

    slot->active = true;
    return BB_OK;
}

static bb_err_t op_set_on(void *st, uint16_t idx, bool on) {
    state_t *s = st;
    s->enabled[idx] = on;
    return BB_OK;
}

static bb_err_t op_set_brightness(void *st, uint16_t idx, uint8_t pct) {
    state_t *s = st;
    s->bri[idx] = (uint8_t)(((uint32_t)pct * 31) / 100);
    return BB_OK;
}

static bb_err_t op_set_color(void *st, uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    state_t *s = st;
    s->rgb[idx*3 + 0] = r;
    s->rgb[idx*3 + 1] = g;
    s->rgb[idx*3 + 2] = b;
    return BB_OK;
}

static bb_err_t op_flush(void *st) {
    return do_flush(st);
}

static bb_err_t op_close(void *st) {
    state_t *s = st;
    s_slots[s->slot].active = false;
    free(s->rgb);
    free(s->bri);
    free(s->enabled);
    free(s->drv);
    free(s);
    return BB_OK;
}

bb_err_t bb_led_apa102_open(const bb_led_apa102_cfg_t *cfg, bb_led_handle_t *out) {
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->pin_clk < 0 || cfg->pin_din < 0) return BB_ERR_INVALID_ARG;
    if (cfg->led_count == 0) return BB_ERR_INVALID_ARG;
    if (cfg->global_brightness_31 > 31) return BB_ERR_INVALID_ARG;

    // Find a free slot.
    int slot = -1;
    for (int i = 0; i < BB_LED_APA102_HOST_MAX_SLOTS; i++) {
        if (!s_slots[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return BB_ERR_NO_SPACE;

    state_t *s = calloc(1, sizeof *s);
    if (!s) return BB_ERR_NO_SPACE;
    s->led_count = cfg->led_count;
    s->rgb = calloc(cfg->led_count * 3, 1);
    s->bri = calloc(cfg->led_count, 1);
    s->enabled = calloc(cfg->led_count, sizeof(bool));
    s->slot = slot;
    if (!s->rgb || !s->bri || !s->enabled) {
        free(s->rgb); free(s->bri); free(s->enabled); free(s);
        return BB_ERR_NO_SPACE;
    }

    // Initialize per-LED brightness to global default.
    for (uint16_t i = 0; i < cfg->led_count; i++) s->bri[i] = cfg->global_brightness_31;

    bb_led_driver_t *drv = calloc(1, sizeof *drv);
    if (!drv) { free(s->rgb); free(s->bri); free(s->enabled); free(s); return BB_ERR_NO_SPACE; }
    *drv = (bb_led_driver_t){
        .set_on = op_set_on,
        .set_brightness = op_set_brightness,
        .set_color = op_set_color,
        .flush = op_flush,
        .close = op_close,
        .caps = (bb_led_caps_t)(BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB),
        .count = cfg->led_count,
    };
    s->drv = drv;

    do_flush(s);  // initial dark state to strip.

    bb_err_t rc = bb_led_handle_create(drv, s, out);
    if (rc != BB_OK) { free(s->rgb); free(s->bri); free(s->enabled); free(drv); free(s); s_slots[slot].active = false; }
    return rc;
}
