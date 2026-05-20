#include "bb_led_pwm.h"
#include "bb_led_driver.h"
#include "bb_log.h"
#include "driver/ledc.h"
#include <stdlib.h>

static const char *TAG = "bb_led_pwm";

#define MAX_CHANNELS 8
static uint8_t s_channel_next = 0;
static bool s_timer_configured = false;
static uint32_t s_timer_freq_hz = 0;
static uint8_t s_timer_resolution = 0;

typedef struct {
    int gpio;
    bool active_low;
    uint8_t channel;
    uint32_t max_duty;
} state_t;

static uint32_t pct_to_duty(const state_t *s, uint8_t pct) {
    if (pct > 100) pct = 100;
    uint32_t d = ((uint32_t)pct * s->max_duty) / 100;
    return s->active_low ? (s->max_duty - d) : d;
}

static bb_err_t apply_duty(state_t *s, uint32_t duty) {
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, s->channel, duty) != ESP_OK) return BB_ERR_INVALID_STATE;
    if (ledc_update_duty(LEDC_LOW_SPEED_MODE, s->channel) != ESP_OK) return BB_ERR_INVALID_STATE;
    return BB_OK;
}

static bb_err_t op_set_on(void *st, uint16_t idx, bool on) {
    (void)idx;
    state_t *s = st;
    return apply_duty(s, pct_to_duty(s, on ? 100 : 0));
}

static bb_err_t op_set_brightness(void *st, uint16_t idx, uint8_t pct) {
    (void)idx;
    state_t *s = st;
    return apply_duty(s, pct_to_duty(s, pct));
}

static bb_err_t op_close(void *st) {
    state_t *s = st;
    ledc_stop(LEDC_LOW_SPEED_MODE, s->channel, s->active_low ? 1 : 0);
    free(s);
    return BB_OK;
}

static const bb_led_driver_t s_drv = {
    .set_on = op_set_on,
    .set_brightness = op_set_brightness,
    .set_color = NULL,
    .flush = NULL,
    .close = op_close,
    .caps = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS,
    .count = 1,
};

bb_err_t bb_led_pwm_open(const bb_led_pwm_cfg_t *cfg, bb_led_handle_t *out) {
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio < 0) return BB_ERR_INVALID_ARG;
    if (cfg->resolution_bits < 1 || cfg->resolution_bits > 14) return BB_ERR_INVALID_ARG;
    if (cfg->freq_hz == 0) return BB_ERR_INVALID_ARG;
    if (s_channel_next >= MAX_CHANNELS) return BB_ERR_NO_SPACE;

    if (!s_timer_configured) {
        ledc_timer_config_t tc = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = (ledc_timer_bit_t)cfg->resolution_bits,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = cfg->freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        if (ledc_timer_config(&tc) != ESP_OK) return BB_ERR_INVALID_STATE;
        s_timer_configured = true;
        s_timer_freq_hz = cfg->freq_hz;
        s_timer_resolution = cfg->resolution_bits;
    } else if (s_timer_freq_hz != cfg->freq_hz || s_timer_resolution != cfg->resolution_bits) {
        bb_log_w(TAG, "shared LEDC timer already configured @ %lu Hz / %u-bit; ignoring requested %lu/%u",
                 (unsigned long)s_timer_freq_hz, (unsigned)s_timer_resolution,
                 (unsigned long)cfg->freq_hz, (unsigned)cfg->resolution_bits);
    }

    state_t *s = calloc(1, sizeof *s);
    if (!s) return BB_ERR_NO_SPACE;
    s->gpio = cfg->gpio;
    s->active_low = cfg->active_low;
    s->channel = s_channel_next++;
    s->max_duty = (1U << s_timer_resolution) - 1U;

    ledc_channel_config_t cc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)s->channel,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = cfg->gpio,
        .duty = pct_to_duty(s, 0),
        .hpoint = 0,
    };
    if (ledc_channel_config(&cc) != ESP_OK) { free(s); return BB_ERR_INVALID_STATE; }

    bb_err_t rc = bb_led_handle_create(&s_drv, s, out);
    if (rc != BB_OK) { ledc_stop(LEDC_LOW_SPEED_MODE, s->channel, 0); free(s); }
    return rc;
}
