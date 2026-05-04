#include "bb_led_gpio.h"
#include "bb_led_gpio_host.h"
#include "bb_led_driver.h"
#include <stdlib.h>

#define BB_LED_GPIO_HOST_MAX_PIN 64
static int s_host_last_level[BB_LED_GPIO_HOST_MAX_PIN];

int bb_led_gpio_host_get_level(int gpio) {
    if (gpio < 0 || gpio >= BB_LED_GPIO_HOST_MAX_PIN) return -1;
    return s_host_last_level[gpio];
}

typedef struct { int gpio; bool active_low; } state_t;

static int level_for_on(const state_t *s, bool on) {
    return s->active_low ? (on ? 0 : 1) : (on ? 1 : 0);
}

static bb_err_t op_set_on(void *st, uint16_t idx, bool on) {
    (void)idx;
    state_t *s = st;
    if (s->gpio < BB_LED_GPIO_HOST_MAX_PIN) s_host_last_level[s->gpio] = level_for_on(s, on);
    return BB_OK;
}

static bb_err_t op_close(void *st) {
    state_t *s = st;
    if (s->gpio < BB_LED_GPIO_HOST_MAX_PIN) s_host_last_level[s->gpio] = -1;
    free(s);
    return BB_OK;
}

static const bb_led_driver_t s_drv = {
    .set_on = op_set_on, .set_brightness = NULL, .set_color = NULL, .flush = NULL, .close = op_close,
    .caps = BB_LED_CAP_ONOFF, .count = 1,
};

bb_err_t bb_led_gpio_open(const bb_led_gpio_cfg_t *cfg, bb_led_handle_t *out) {
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio < 0 || cfg->gpio >= BB_LED_GPIO_HOST_MAX_PIN) return BB_ERR_INVALID_ARG;
    state_t *s = calloc(1, sizeof *s);
    if (!s) return BB_ERR_NO_SPACE;
    s->gpio = cfg->gpio;
    s->active_low = cfg->active_low;
    s_host_last_level[s->gpio] = level_for_on(s, false);
    return bb_led_handle_create(&s_drv, s, out);
}
