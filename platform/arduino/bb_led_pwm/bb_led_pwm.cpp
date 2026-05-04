extern "C" {
#include "bb_led_pwm.h"
#include "bb_led_driver.h"
#include <stdlib.h>
}
#include <Arduino.h>

extern "C" {

typedef struct {
    int gpio;
    bool active_low;
} state_t;

static uint8_t pct_to_duty8(const state_t *s, uint8_t pct) {
    if (pct > 100) pct = 100;
    uint16_t d = ((uint16_t)pct * 255U) / 100U;
    return s->active_low ? (uint8_t)(255U - d) : (uint8_t)d;
}

static bb_err_t op_set_on(void *st, uint16_t idx, bool on) {
    (void)idx;
    state_t *s = (state_t*)st;
    analogWrite(s->gpio, pct_to_duty8(s, on ? 100 : 0));
    return BB_OK;
}

static bb_err_t op_set_brightness(void *st, uint16_t idx, uint8_t pct) {
    (void)idx;
    state_t *s = (state_t*)st;
    analogWrite(s->gpio, pct_to_duty8(s, pct));
    return BB_OK;
}

static bb_err_t op_close(void *st) {
    state_t *s = (state_t*)st;
    analogWrite(s->gpio, s->active_low ? 255 : 0);
    pinMode(s->gpio, INPUT);
    free(s);
    return BB_OK;
}

static const bb_led_driver_t s_drv = {
    .set_on = op_set_on, .set_brightness = op_set_brightness,
    .set_color = NULL, .flush = NULL, .close = op_close,
    .caps = (bb_led_caps_t)(BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS), .count = 1,
};

bb_err_t bb_led_pwm_open(const bb_led_pwm_cfg_t *cfg, bb_led_handle_t *out) {
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio < 0) return BB_ERR_INVALID_ARG;
    state_t *s = (state_t*)calloc(1, sizeof *s);
    if (!s) return BB_ERR_NO_SPACE;
    s->gpio = cfg->gpio;
    s->active_low = cfg->active_low;
    pinMode(s->gpio, OUTPUT);
    analogWrite(s->gpio, pct_to_duty8(s, 0));
    return bb_led_handle_create(&s_drv, s, out);
}

} // extern "C"
