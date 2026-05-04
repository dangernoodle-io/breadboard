#include "bb_led_gpio.h"
#include "bb_led_driver.h"
#include "driver/gpio.h"
#include <stdlib.h>

typedef struct {
    int gpio;
    bool active_low;
} state_t;

static int level_for_on(const state_t *s, bool on) {
    return s->active_low ? (on ? 0 : 1) : (on ? 1 : 0);
}

static bb_err_t op_set_on(void *st, uint16_t idx, bool on) {
    (void)idx;
    state_t *s = st;
    return gpio_set_level(s->gpio, level_for_on(s, on)) == ESP_OK ? BB_OK : BB_ERR_INVALID_STATE;
}

static bb_err_t op_close(void *st) {
    state_t *s = st;
    gpio_reset_pin(s->gpio);
    free(s);
    return BB_OK;
}

static const bb_led_driver_t s_drv = {
    .set_on = op_set_on,
    .set_brightness = NULL,
    .set_color = NULL,
    .flush = NULL,
    .close = op_close,
    .caps = BB_LED_CAP_ONOFF,
    .count = 1,
};

bb_err_t bb_led_gpio_open(const bb_led_gpio_cfg_t *cfg, bb_led_handle_t *out) {
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio < 0) return BB_ERR_INVALID_ARG;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) return BB_ERR_INVALID_STATE;

    state_t *s = calloc(1, sizeof *s);
    if (!s) return BB_ERR_NO_SPACE;
    s->gpio = cfg->gpio;
    s->active_low = cfg->active_low;

    // Drive OFF initially.
    gpio_set_level(s->gpio, level_for_on(s, false));

    bb_err_t rc = bb_led_handle_create(&s_drv, s, out);
    if (rc != BB_OK) { free(s); gpio_reset_pin(s->gpio); }
    return rc;
}
