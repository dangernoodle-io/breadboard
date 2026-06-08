// Tests for bb_led_rgb_pwm — 3-GPIO common-anode RGB PWM LED driver.
#include "unity.h"
#include "bb_led_rgb_pwm.h"
#include "bb_led_rgb_pwm_host.h"
#include "bb_led_gamma.h"

// Default cfg: active_low (common anode), gpio_r=22 g=16 b=17.
static bb_led_rgb_pwm_cfg_t s_cfg_default = {
    .gpio_r = 22, .gpio_g = 16, .gpio_b = 17,
    .freq_hz = 5000, .resolution_bits = 8, .active_low = true,
};

// 1: open/close, caps/count/name
void test_rgb_pwm_open_close(void)
{
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&s_cfg_default, &h));
    TEST_ASSERT_EQUAL(BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB, bb_led_caps(h));
    TEST_ASSERT_EQUAL_UINT16(1, bb_led_count(h));
    TEST_ASSERT_EQUAL_STRING("rgb_pwm", bb_led_name(h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_close(h));
}

// 2: set_color returns BB_OK (not BB_ERR_UNSUPPORTED)
void test_rgb_pwm_set_color_accepted(void)
{
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&s_cfg_default, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 255, 0, 0));
    bb_led_close(h);
}

// 3: set_color writes all three channels; active_low inversion per channel
// color(255,0,0) active_low: R full on → duty 0; G/B off → duty 65535.
// base(gpio_r) == 255 (raw component, not inverted).
void test_rgb_pwm_set_color_writes_all_three_channels(void)
{
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&s_cfg_default, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 255, 0, 0));

    // R channel (gpio 22): component=255, env=65535 → lin=65535, duty=65535-65535=0
    TEST_ASSERT_EQUAL_INT(0, (int)bb_led_rgb_pwm_host_get_duty(22));
    TEST_ASSERT_EQUAL_INT(255, bb_led_rgb_pwm_host_get_base(22));

    // G channel (gpio 16): component=0, env=65535 → lin=0, duty=65535-0=65535
    TEST_ASSERT_EQUAL_INT(65535, (int)bb_led_rgb_pwm_host_get_duty(16));

    // B channel (gpio 17): same as G
    TEST_ASSERT_EQUAL_INT(65535, (int)bb_led_rgb_pwm_host_get_duty(17));

    bb_led_close(h);
}

// 4: active_low inversion — white → all duty 0; black → all duty 65535
void test_rgb_pwm_active_low_inversion_per_channel(void)
{
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&s_cfg_default, &h));

    // white (255,255,255): full on active_low → duty 0 on all
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 255, 255, 255));
    TEST_ASSERT_EQUAL_INT(0, (int)bb_led_rgb_pwm_host_get_duty(22));
    TEST_ASSERT_EQUAL_INT(0, (int)bb_led_rgb_pwm_host_get_duty(16));
    TEST_ASSERT_EQUAL_INT(0, (int)bb_led_rgb_pwm_host_get_duty(17));

    // black (0,0,0): off active_low → duty 65535 on all
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT(65535, (int)bb_led_rgb_pwm_host_get_duty(22));
    TEST_ASSERT_EQUAL_INT(65535, (int)bb_led_rgb_pwm_host_get_duty(16));
    TEST_ASSERT_EQUAL_INT(65535, (int)bb_led_rgb_pwm_host_get_duty(17));

    bb_led_close(h);
}

// 5: active_high no inversion — color(255,0,0) → R duty 65535, G/B duty 0
void test_rgb_pwm_active_high_no_inversion(void)
{
    bb_led_rgb_pwm_cfg_t cfg = {
        .gpio_r = 10, .gpio_g = 11, .gpio_b = 12,
        .freq_hz = 5000, .resolution_bits = 8, .active_low = false,
    };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 255, 0, 0));
    TEST_ASSERT_EQUAL_INT(65535, (int)bb_led_rgb_pwm_host_get_duty(10));
    TEST_ASSERT_EQUAL_INT(0,     (int)bb_led_rgb_pwm_host_get_duty(11));
    TEST_ASSERT_EQUAL_INT(0,     (int)bb_led_rgb_pwm_host_get_duty(12));
    bb_led_close(h);
}

// 6: set_level applies gamma to color; env < level input; only R has non-zero duty
void test_rgb_pwm_set_level_applies_gamma_to_color(void)
{
    bb_led_rgb_pwm_cfg_t cfg = {
        .gpio_r = 20, .gpio_g = 21, .gpio_b = 23,
        .freq_hz = 5000, .resolution_bits = 8, .active_low = false,
    };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 255, 0, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 32768));

    long env_got = bb_led_rgb_pwm_host_get_env(20);
    TEST_ASSERT_TRUE(env_got >= 0);
    TEST_ASSERT_TRUE(env_got < 32768);  // CIE gamma compresses mid-range

    long r_duty = bb_led_rgb_pwm_host_get_duty(20);
    long g_duty = bb_led_rgb_pwm_host_get_duty(21);
    TEST_ASSERT_TRUE(r_duty > 0);
    TEST_ASSERT_EQUAL_INT(0, (int)g_duty);

    bb_led_close(h);
}

// 7: set_level preserves hue — only the lit channel has non-zero duty
void test_rgb_pwm_set_level_preserves_hue(void)
{
    bb_led_rgb_pwm_cfg_t cfg = {
        .gpio_r = 30, .gpio_g = 31, .gpio_b = 32,
        .freq_hz = 5000, .resolution_bits = 8, .active_low = false,
    };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0, 255, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 32768));

    TEST_ASSERT_EQUAL_INT(0, (int)bb_led_rgb_pwm_host_get_duty(30));  // R
    TEST_ASSERT_EQUAL_INT(0, (int)bb_led_rgb_pwm_host_get_duty(32));  // B
    long g_duty = bb_led_rgb_pwm_host_get_duty(31);
    TEST_ASSERT_TRUE(g_duty > 0);

    bb_led_close(h);
}

// 8: set_color after set_level uses the stored level's gamma env
void test_rgb_pwm_set_color_after_set_level_uses_stored_level(void)
{
    bb_led_rgb_pwm_cfg_t cfg = {
        .gpio_r = 40, .gpio_g = 41, .gpio_b = 42,
        .freq_hz = 5000, .resolution_bits = 8, .active_low = false,
    };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 32768));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 255, 0, 0));

    long expected_env = (long)bb_led_gamma_cie(32768);
    TEST_ASSERT_EQUAL_INT((int)expected_env, (int)bb_led_rgb_pwm_host_get_env(40));

    bb_led_close(h);
}

// 9: set_brightness(50) → env == 50*65535/100 == 32767
void test_rgb_pwm_set_brightness(void)
{
    bb_led_rgb_pwm_cfg_t cfg = {
        .gpio_r = 50, .gpio_g = 51, .gpio_b = 52,
        .freq_hz = 5000, .resolution_bits = 8, .active_low = false,
    };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 255, 255, 255));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 50));

    TEST_ASSERT_EQUAL_INT(32767, (int)bb_led_rgb_pwm_host_get_env(50));

    bb_led_close(h);
}

// 10: set_on(true) lights LED; set_on(false) turns off all channels
void test_rgb_pwm_set_on_and_off(void)
{
    bb_led_rgb_pwm_cfg_t cfg = {
        .gpio_r = 3, .gpio_g = 4, .gpio_b = 5,
        .freq_hz = 5000, .resolution_bits = 8, .active_low = false,
    };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 255, 255, 255));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, true));
    TEST_ASSERT_TRUE(bb_led_rgb_pwm_host_get_duty(3) > 0);

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, false));
    TEST_ASSERT_EQUAL_INT(0, (int)bb_led_rgb_pwm_host_get_duty(3));
    TEST_ASSERT_EQUAL_INT(0, (int)bb_led_rgb_pwm_host_get_duty(4));
    TEST_ASSERT_EQUAL_INT(0, (int)bb_led_rgb_pwm_host_get_duty(5));

    bb_led_close(h);
}

// 11: initial state off — active_low: open records env=0, duty=65535 on all channels
void test_rgb_pwm_initial_state_off(void)
{
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&s_cfg_default, &h));

    // env=0, active_low: duty = 65535 - 0 = 65535
    TEST_ASSERT_EQUAL_INT(65535, (int)bb_led_rgb_pwm_host_get_duty(22));
    TEST_ASSERT_EQUAL_INT(65535, (int)bb_led_rgb_pwm_host_get_duty(16));
    TEST_ASSERT_EQUAL_INT(65535, (int)bb_led_rgb_pwm_host_get_duty(17));

    bb_led_close(h);
}

// 12: invalid args → BB_ERR_INVALID_ARG
void test_rgb_pwm_invalid_args(void)
{
    bb_led_handle_t h;
    bb_led_rgb_pwm_cfg_t cfg = s_cfg_default;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_rgb_pwm_open(NULL, &h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_rgb_pwm_open(&cfg, NULL));

    cfg = s_cfg_default;
    cfg.gpio_r = -1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_rgb_pwm_open(&cfg, &h));

    cfg = s_cfg_default;
    cfg.resolution_bits = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_rgb_pwm_open(&cfg, &h));

    cfg = s_cfg_default;
    cfg.resolution_bits = 15;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_rgb_pwm_open(&cfg, &h));

    cfg = s_cfg_default;
    cfg.freq_hz = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_rgb_pwm_open(&cfg, &h));
}

// 13: bb_led_fill_color returns BB_OK
void test_rgb_pwm_fill_color(void)
{
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_rgb_pwm_open(&s_cfg_default, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_fill_color(h, 0xAA, 0xBB, 0xCC));
    bb_led_close(h);
}
