#include "bb_led_rgb_pwm.h"
#include "bb_led_driver.h"
#include "bb_led_gamma.h"
#include "bb_log.h"
#include "driver/ledc.h"
#include <stdlib.h>

static const char *TAG = "bb_led_rgb_pwm";

#define MAX_CHANNELS 8
static uint8_t s_channel_next = 0;
static bool s_timer_configured = false;
static uint32_t s_timer_freq_hz = 0;
static uint8_t s_timer_resolution = 0;

typedef struct {
    uint8_t ch_r, ch_g, ch_b;
    bool active_low;
    uint32_t max_duty;
    uint8_t base_r, base_g, base_b;
    uint16_t last_level;
    bool level_set;
} state_t;

// Compute per-channel duty: apply color × env envelope.
// component8: 0..255 base color component.
// env16: 0..65535 envelope (gamma-corrected level or direct brightness env).
// Returns duty value for the LEDC hardware (after active_low inversion).
static uint32_t component_duty(const state_t *s, uint8_t component8, uint16_t env16) {
    // lin = component * env / 255, range 0..65535
    uint32_t lin = (uint32_t)component8 * (uint32_t)env16 / 255u;
    // d = lin * max_duty / 65535, range 0..max_duty
    uint32_t d = (uint32_t)((uint64_t)lin * s->max_duty / 65535u);
    return s->active_low ? (s->max_duty - d) : d;
}

static void write_rgb(state_t *s, uint8_t r, uint8_t g, uint8_t b, uint16_t env) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_r, component_duty(s, r, env));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_r);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_g, component_duty(s, g, env));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_g);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_b, component_duty(s, b, env));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_b);
}

static bb_err_t op_set_color(void *st, uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    (void)idx;
    state_t *s = st;
    s->base_r = r;
    s->base_g = g;
    s->base_b = b;
    uint16_t env = s->level_set ? (uint16_t)bb_led_gamma_cie(s->last_level) : 65535u;
    write_rgb(s, s->base_r, s->base_g, s->base_b, env);
    return BB_OK;
}

static bb_err_t op_set_level(void *st, uint16_t idx, uint16_t level) {
    (void)idx;
    state_t *s = st;
    s->last_level = level;
    s->level_set = true;
    uint16_t env = (uint16_t)bb_led_gamma_cie(level);
    write_rgb(s, s->base_r, s->base_g, s->base_b, env);
    return BB_OK;
}

static bb_err_t op_set_brightness(void *st, uint16_t idx, uint8_t pct) {
    (void)idx;
    state_t *s = st;
    if (pct > 100) pct = 100;
    uint16_t env = (uint16_t)((uint32_t)pct * 65535u / 100u);
    s->level_set = false;
    write_rgb(s, s->base_r, s->base_g, s->base_b, env);
    return BB_OK;
}

static bb_err_t op_set_on(void *st, uint16_t idx, bool on) {
    (void)idx;
    state_t *s = st;
    uint16_t env = on ? 65535u : 0u;
    s->level_set = false;
    write_rgb(s, s->base_r, s->base_g, s->base_b, env);
    return BB_OK;
}

static bb_err_t op_close(void *st) {
    state_t *s = st;
    int idle = s->active_low ? 1 : 0;
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_r, idle);
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_g, idle);
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_b, idle);
    free(s);
    return BB_OK;
}

static const bb_led_driver_t s_drv = {
    .set_on = op_set_on,
    .set_brightness = op_set_brightness,
    .set_level = op_set_level,
    .set_color = op_set_color,
    .flush = NULL,
    .close = op_close,
    .caps = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB,
    .count = 1,
    .name = "rgb_pwm",
};

bb_err_t bb_led_rgb_pwm_open(const bb_led_rgb_pwm_cfg_t *cfg, bb_led_handle_t *out) {
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio_r < 0 || cfg->gpio_g < 0 || cfg->gpio_b < 0) return BB_ERR_INVALID_ARG;
    if (cfg->resolution_bits < 1 || cfg->resolution_bits > 14) return BB_ERR_INVALID_ARG;
    if (cfg->freq_hz == 0) return BB_ERR_INVALID_ARG;
    if (s_channel_next + 3 > MAX_CHANNELS) return BB_ERR_NO_SPACE;

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
    s->active_low = cfg->active_low;
    s->max_duty = (1U << s_timer_resolution) - 1U;
    s->ch_r = s_channel_next++;
    s->ch_g = s_channel_next++;
    s->ch_b = s_channel_next++;

    uint32_t off_duty = cfg->active_low ? s->max_duty : 0u;
    int gpios[3] = { cfg->gpio_r, cfg->gpio_g, cfg->gpio_b };
    uint8_t channels[3] = { s->ch_r, s->ch_g, s->ch_b };
    for (int i = 0; i < 3; i++) {
        ledc_channel_config_t cc = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)channels[i],
            .timer_sel = LEDC_TIMER_0,
            .intr_type = LEDC_INTR_DISABLE,
            .gpio_num = gpios[i],
            .duty = off_duty,
            .hpoint = 0,
        };
        if (ledc_channel_config(&cc) != ESP_OK) {
            free(s);
            return BB_ERR_INVALID_STATE;
        }
    }

    bb_err_t rc = bb_led_handle_create(&s_drv, s, out);
    if (rc != BB_OK) {
        int idle = cfg->active_low ? 1 : 0;
        ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_r, idle);
        ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_g, idle);
        ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s->ch_b, idle);
        free(s);
    }
    return rc;
}
