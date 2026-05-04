#include "bb_led_pwm.h"
#include "bb_led_driver.h"
#include "bb_led_pwm_host.h"
#include <stdlib.h>

#define BB_LED_PWM_HOST_MAX_PIN 64
static int s_host_last_pct[BB_LED_PWM_HOST_MAX_PIN];
static bool s_host_seen[BB_LED_PWM_HOST_MAX_PIN];

int bb_led_pwm_host_get_pct(int gpio) {
    if (gpio < 0 || gpio >= BB_LED_PWM_HOST_MAX_PIN) return -1;
    return s_host_seen[gpio] ? s_host_last_pct[gpio] : -1;
}

void bb_led_pwm_test_reset(void) {
    for (int i = 0; i < BB_LED_PWM_HOST_MAX_PIN; i++) {
        s_host_last_pct[i] = 0;
        s_host_seen[i] = false;
    }
}

typedef struct { int gpio; bool active_low; } state_t;

static uint8_t apply_pct(const state_t *s, uint8_t pct) {
    return s->active_low ? (uint8_t)(100 - pct) : pct;
}

static bb_err_t op_set_on(void *st, uint16_t idx, bool on) {
    (void)idx;
    state_t *s = st;
    int eff = apply_pct(s, on ? 100 : 0);
    if (s->gpio < BB_LED_PWM_HOST_MAX_PIN) { s_host_last_pct[s->gpio] = eff; s_host_seen[s->gpio] = true; }
    return BB_OK;
}

static bb_err_t op_set_brightness(void *st, uint16_t idx, uint8_t pct) {
    (void)idx;
    state_t *s = st;
    int eff = apply_pct(s, pct);
    if (s->gpio < BB_LED_PWM_HOST_MAX_PIN) { s_host_last_pct[s->gpio] = eff; s_host_seen[s->gpio] = true; }
    return BB_OK;
}

static bb_err_t op_close(void *st) {
    state_t *s = st;
    if (s->gpio < BB_LED_PWM_HOST_MAX_PIN) s_host_seen[s->gpio] = false;
    free(s);
    return BB_OK;
}

static const bb_led_driver_t s_drv = {
    .set_on = op_set_on, .set_brightness = op_set_brightness,
    .set_color = NULL, .flush = NULL, .close = op_close,
    .caps = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS, .count = 1,
};

bb_err_t bb_led_pwm_open(const bb_led_pwm_cfg_t *cfg, bb_led_handle_t *out) {
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio < 0 || cfg->gpio >= BB_LED_PWM_HOST_MAX_PIN) return BB_ERR_INVALID_ARG;
    if (cfg->resolution_bits < 1 || cfg->resolution_bits > 14) return BB_ERR_INVALID_ARG;
    if (cfg->freq_hz == 0) return BB_ERR_INVALID_ARG;
    state_t *s = calloc(1, sizeof *s);
    if (!s) return BB_ERR_NO_SPACE;
    s->gpio = cfg->gpio;
    s->active_low = cfg->active_low;
    int eff = apply_pct(s, 0);
    s_host_last_pct[s->gpio] = eff;
    s_host_seen[s->gpio] = true;
    return bb_led_handle_create(&s_drv, s, out);
}
