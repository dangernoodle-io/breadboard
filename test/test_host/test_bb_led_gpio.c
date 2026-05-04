// Tests for bb_led_gpio single-GPIO driver.
#include "unity.h"
#include "bb_led_gpio.h"
#include "bb_led_gpio_host.h"

void test_gpio_open_close(void)
{
    bb_led_gpio_cfg_t cfg = { .gpio = 2, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_gpio_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_LED_CAP_ONOFF, bb_led_caps(h));
    TEST_ASSERT_EQUAL_UINT16(1, bb_led_count(h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_close(h));
}

void test_gpio_active_high_set_on(void)
{
    bb_led_gpio_cfg_t cfg = { .gpio = 3, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_gpio_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, true));
    TEST_ASSERT_EQUAL_INT(1, bb_led_gpio_host_get_level(3));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, false));
    TEST_ASSERT_EQUAL_INT(0, bb_led_gpio_host_get_level(3));

    bb_led_close(h);
}

void test_gpio_active_low_set_on(void)
{
    bb_led_gpio_cfg_t cfg = { .gpio = 4, .active_low = true };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_gpio_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, true));
    TEST_ASSERT_EQUAL_INT(0, bb_led_gpio_host_get_level(4));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, false));
    TEST_ASSERT_EQUAL_INT(1, bb_led_gpio_host_get_level(4));

    bb_led_close(h);
}

void test_gpio_idx_must_be_zero(void)
{
    bb_led_gpio_cfg_t cfg = { .gpio = 5, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_gpio_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_on(h, 1, true));
    bb_led_close(h);
}

void test_gpio_initial_state_off(void)
{
    bb_led_gpio_cfg_t cfg = { .gpio = 6, .active_low = false };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_gpio_open(&cfg, &h));
    // After open, level should be 0 (off) for active_high.
    TEST_ASSERT_EQUAL_INT(0, bb_led_gpio_host_get_level(6));
    bb_led_close(h);
}

void test_gpio_initial_state_off_active_low(void)
{
    bb_led_gpio_cfg_t cfg = { .gpio = 7, .active_low = true };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_gpio_open(&cfg, &h));
    // After open, level should be 1 (off = HIGH) for active_low.
    TEST_ASSERT_EQUAL_INT(1, bb_led_gpio_host_get_level(7));
    bb_led_close(h);
}

void test_gpio_invalid_args(void)
{
    bb_led_gpio_cfg_t cfg = { .gpio = 8, .active_low = false };
    bb_led_handle_t h;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_gpio_open(NULL, &h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_gpio_open(&cfg, NULL));

    cfg.gpio = -1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_gpio_open(&cfg, &h));
}
