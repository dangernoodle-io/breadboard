// Tests for bb_button_gpio host driver.
#include "unity.h"
#include "bb_button_gpio.h"
#include "bb_button_gpio_host.h"

static bb_button_handle_t open_gpio_btn(int gpio, bool active_low, uint16_t debounce_ms)
{
    bb_button_gpio_cfg_t cfg = {
        .gpio        = gpio,
        .active_low  = active_low,
        .debounce_ms = debounce_ms,
        .prefer_isr  = false,
    };
    bb_button_handle_t h = NULL;
    bb_button_gpio_open(&cfg, &h);
    return h;
}

void test_btn_gpio_open_close(void)
{
    bb_button_handle_t h = open_gpio_btn(20, true, 25);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL(BB_OK, bb_button_close(h));
}

void test_btn_gpio_initial_state_not_pressed(void)
{
    bb_button_handle_t h = open_gpio_btn(21, true, 25);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_FALSE(bb_button_is_pressed(h));
    bb_button_close(h);
}

void test_btn_gpio_inject_sets_pressed(void)
{
    bb_button_handle_t h = open_gpio_btn(22, true, 25);
    TEST_ASSERT_NOT_NULL(h);
    bb_button_host_inject_edge(h, true, 30);
    TEST_ASSERT_TRUE(bb_button_is_pressed(h));
    bb_button_close(h);
}

void test_btn_gpio_inject_sets_released(void)
{
    bb_button_handle_t h = open_gpio_btn(23, true, 25);
    TEST_ASSERT_NOT_NULL(h);
    bb_button_host_inject_edge(h, true, 30);
    bb_button_host_inject_edge(h, false, 60);
    TEST_ASSERT_FALSE(bb_button_is_pressed(h));
    bb_button_close(h);
}

void test_btn_gpio_invalid_args(void)
{
    bb_button_handle_t h = NULL;
    bb_button_gpio_cfg_t cfg = { .gpio = 24, .active_low = true, .debounce_ms = 25, .prefer_isr = false };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_button_gpio_open(NULL, &h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_button_gpio_open(&cfg, NULL));
    cfg.gpio = -1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_button_gpio_open(&cfg, &h));
}

void test_btn_gpio_poll_noop_on_host(void)
{
    bb_button_handle_t h = open_gpio_btn(25, true, 25);
    TEST_ASSERT_NOT_NULL(h);
    // poll() should return BB_OK without crashing.
    TEST_ASSERT_EQUAL(BB_OK, bb_button_poll(h));
    bb_button_close(h);
}
