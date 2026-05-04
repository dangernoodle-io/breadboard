extern "C" {
#include "bb_led_gpio.h"
#include "bb_led_driver.h"
#include <stdlib.h>
}
#include <Arduino.h>

extern "C" {

typedef struct { int gpio; bool active_low; } state_t;

static int level_for_on(const state_t *s, bool on) {
    return s->active_low ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
}

static bb_err_t op_set_on(void *st, uint16_t idx, bool on) {
    (void)idx;
    state_t *s = (state_t*)st;
    digitalWrite(s->gpio, level_for_on(s, on));
    return BB_OK;
}

static bb_err_t op_close(void *st) {
    state_t *s = (state_t*)st;
    pinMode(s->gpio, INPUT);
    free(s);
    return BB_OK;
}

static const bb_led_driver_t s_drv = {
    .set_on = op_set_on, .set_brightness = NULL, .set_color = NULL, .flush = NULL, .close = op_close,
    .caps = BB_LED_CAP_ONOFF, .count = 1,
};

bb_err_t bb_led_gpio_open(const bb_led_gpio_cfg_t *cfg, bb_led_handle_t *out) {
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio < 0) return BB_ERR_INVALID_ARG;
    state_t *s = (state_t*)calloc(1, sizeof *s);
    if (!s) return BB_ERR_NO_SPACE;
    s->gpio = cfg->gpio;
    s->active_low = cfg->active_low;
    pinMode(s->gpio, OUTPUT);
    digitalWrite(s->gpio, level_for_on(s, false));
    return bb_led_handle_create(&s_drv, s, out);
}

}
