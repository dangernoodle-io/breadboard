// Tests for bb_led_pwm brightness-controlled LED driver.
#include "unity.h"
#include "bb_led_pwm.h"
#include "bb_led_pwm_host.h"

void test_pwm_open_close(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 5, .freq_hz = 5000, .resolution_bits = 8, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_pwm_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS, bb_led_caps(h));
    TEST_ASSERT_EQUAL_UINT16(1, bb_led_count(h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_close(h));
}

void test_pwm_set_brightness_active_high(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 5, .freq_hz = 5000, .resolution_bits = 8, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_pwm_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 50));
    TEST_ASSERT_EQUAL_INT(50, bb_led_pwm_host_get_pct(5));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 100));
    TEST_ASSERT_EQUAL_INT(100, bb_led_pwm_host_get_pct(5));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, bb_led_pwm_host_get_pct(5));

    bb_led_close(h);
}

void test_pwm_set_brightness_active_low(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 6, .freq_hz = 5000, .resolution_bits = 8, .active_low = true };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_pwm_open(&cfg, &h));

    // active_low inverts: set_brightness(50) → 100-50=50
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 50));
    TEST_ASSERT_EQUAL_INT(50, bb_led_pwm_host_get_pct(6));

    // set_brightness(100) → 100-100=0
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 100));
    TEST_ASSERT_EQUAL_INT(0, bb_led_pwm_host_get_pct(6));

    // set_brightness(0) → 100-0=100
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 0));
    TEST_ASSERT_EQUAL_INT(100, bb_led_pwm_host_get_pct(6));

    bb_led_close(h);
}

void test_pwm_set_on_active_high(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 7, .freq_hz = 5000, .resolution_bits = 8, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_pwm_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, true));
    TEST_ASSERT_EQUAL_INT(100, bb_led_pwm_host_get_pct(7));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, false));
    TEST_ASSERT_EQUAL_INT(0, bb_led_pwm_host_get_pct(7));

    bb_led_close(h);
}

void test_pwm_set_on_active_low(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 8, .freq_hz = 5000, .resolution_bits = 8, .active_low = true };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_pwm_open(&cfg, &h));

    // set_on(true) → 100-100=0
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, true));
    TEST_ASSERT_EQUAL_INT(0, bb_led_pwm_host_get_pct(8));

    // set_on(false) → 100-0=100
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, false));
    TEST_ASSERT_EQUAL_INT(100, bb_led_pwm_host_get_pct(8));

    bb_led_close(h);
}

void test_pwm_set_color_unsupported(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 9, .freq_hz = 5000, .resolution_bits = 8, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_pwm_open(&cfg, &h));
    // PWM driver caps lack RGB; set_color must be unsupported
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_led_set_color(h, 0, 255, 128, 64));
    bb_led_close(h);
}

void test_pwm_idx_must_be_zero(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 10, .freq_hz = 5000, .resolution_bits = 8, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_pwm_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_brightness(h, 1, 50));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_on(h, 1, true));
    bb_led_close(h);
}

void test_pwm_invalid_args(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 11, .freq_hz = 5000, .resolution_bits = 8, .active_low = false };
    bb_led_handle_t h;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_pwm_open(NULL, &h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_pwm_open(&cfg, NULL));

    cfg.gpio = -1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_pwm_open(&cfg, &h));

    cfg.gpio = 11;
    cfg.resolution_bits = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_pwm_open(&cfg, &h));

    cfg.resolution_bits = 15;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_pwm_open(&cfg, &h));

    cfg.resolution_bits = 8;
    cfg.freq_hz = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_pwm_open(&cfg, &h));
}

void test_pwm_initial_state_off_active_high(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 12, .freq_hz = 5000, .resolution_bits = 8, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_pwm_open(&cfg, &h));
    // After open, pct should be 0 (off) for active_high.
    TEST_ASSERT_EQUAL_INT(0, bb_led_pwm_host_get_pct(12));
    bb_led_close(h);
}

void test_pwm_initial_state_off_active_low(void)
{
    bb_led_pwm_cfg_t cfg = { .gpio = 13, .freq_hz = 5000, .resolution_bits = 8, .active_low = true };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_pwm_open(&cfg, &h));
    // After open, pct should be 100 (off = full duty inverted) for active_low.
    TEST_ASSERT_EQUAL_INT(100, bb_led_pwm_host_get_pct(13));
    bb_led_close(h);
}
