#include "bb_led_rgb_pwm.h"
#include "bb_led_driver.h"
#include "bb_led_gamma.h"
#include "bb_led_rgb_pwm_host.h"
#include <stdlib.h>

#define BB_LED_RGB_PWM_HOST_MAX_PIN 64

// Per-GPIO tracked state (normalized to 0..65535 duty).
static long s_host_duty[BB_LED_RGB_PWM_HOST_MAX_PIN];
static int  s_host_base[BB_LED_RGB_PWM_HOST_MAX_PIN];
static long s_host_env[BB_LED_RGB_PWM_HOST_MAX_PIN];
static bool s_host_seen[BB_LED_RGB_PWM_HOST_MAX_PIN];

long bb_led_rgb_pwm_host_get_duty(int gpio) {
    if (gpio < 0 || gpio >= BB_LED_RGB_PWM_HOST_MAX_PIN) return -1;
    return s_host_seen[gpio] ? s_host_duty[gpio] : -1;
}

int bb_led_rgb_pwm_host_get_base(int gpio) {
    if (gpio < 0 || gpio >= BB_LED_RGB_PWM_HOST_MAX_PIN) return -1;
    return s_host_seen[gpio] ? s_host_base[gpio] : -1;
}

long bb_led_rgb_pwm_host_get_env(int gpio) {
    if (gpio < 0 || gpio >= BB_LED_RGB_PWM_HOST_MAX_PIN) return -1;
    return s_host_seen[gpio] ? s_host_env[gpio] : -1;
}

void bb_led_rgb_pwm_host_reset(void) {
    for (int i = 0; i < BB_LED_RGB_PWM_HOST_MAX_PIN; i++) {
        s_host_duty[i] = 0;
        s_host_base[i] = 0;
        s_host_env[i] = 0;
        s_host_seen[i] = false;
    }
}

void bb_led_rgb_pwm_host_test_reset(void) {
    bb_led_rgb_pwm_host_reset();
}

// Internal state for a single handle.
typedef struct {
    int gpio_r, gpio_g, gpio_b;
    bool active_low;
    uint8_t base_r, base_g, base_b;
    uint16_t last_level;
    bool level_set;
} state_t;

// Record duty + base + env for a single GPIO.
// lin = component8 * env16 / 255, already computed 0..65535 range.
static void record(int gpio, uint8_t component8, uint16_t env16, bool active_low) {
    if (gpio < 0 || gpio >= BB_LED_RGB_PWM_HOST_MAX_PIN) return;
    // On host max_duty == 65535, so duty == lin.
    long lin = (long)((uint32_t)component8 * (uint32_t)env16 / 255u);
    long duty = active_low ? (65535L - lin) : lin;
    s_host_duty[gpio] = duty;
    s_host_base[gpio] = (int)component8;
    s_host_env[gpio]  = (long)env16;
    s_host_seen[gpio] = true;
}

static void write_rgb(state_t *s, uint8_t r, uint8_t g, uint8_t b, uint16_t env) {
    record(s->gpio_r, r, env, s->active_low);
    record(s->gpio_g, g, env, s->active_low);
    record(s->gpio_b, b, env, s->active_low);
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
    if (s->gpio_r < BB_LED_RGB_PWM_HOST_MAX_PIN) s_host_seen[s->gpio_r] = false;
    if (s->gpio_g < BB_LED_RGB_PWM_HOST_MAX_PIN) s_host_seen[s->gpio_g] = false;
    if (s->gpio_b < BB_LED_RGB_PWM_HOST_MAX_PIN) s_host_seen[s->gpio_b] = false;
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
    if (cfg->gpio_r < 0 || cfg->gpio_r >= BB_LED_RGB_PWM_HOST_MAX_PIN) return BB_ERR_INVALID_ARG;
    if (cfg->gpio_g < 0 || cfg->gpio_g >= BB_LED_RGB_PWM_HOST_MAX_PIN) return BB_ERR_INVALID_ARG;
    if (cfg->gpio_b < 0 || cfg->gpio_b >= BB_LED_RGB_PWM_HOST_MAX_PIN) return BB_ERR_INVALID_ARG;
    if (cfg->resolution_bits < 1 || cfg->resolution_bits > 14) return BB_ERR_INVALID_ARG;
    if (cfg->freq_hz == 0) return BB_ERR_INVALID_ARG;

    state_t *s = calloc(1, sizeof *s);
    if (!s) return BB_ERR_NO_SPACE;
    s->gpio_r = cfg->gpio_r;
    s->gpio_g = cfg->gpio_g;
    s->gpio_b = cfg->gpio_b;
    s->active_low = cfg->active_low;

    // Record initial off state for all three channels.
    write_rgb(s, 0, 0, 0, 0u);

    bb_err_t rc = bb_led_handle_create(&s_drv, s, out);
    if (rc != BB_OK) free(s);
    return rc;
}
