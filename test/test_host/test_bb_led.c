// Tests for bb_led capability-aware dispatch using a mock driver.
#include "unity.h"
#include "bb_led.h"
#include "bb_led_driver.h"
#include <string.h>

typedef struct {
    bool    on[8];
    uint8_t pct[8];
    uint8_t r[8], g[8], b[8];
    bool    flushed;
    bool    closed;
} mock_state_t;

static mock_state_t g_mock;

static bb_err_t m_set_on        (void *s, uint16_t i, bool v)                          { ((mock_state_t *)s)->on[i] = v; return BB_OK; }
static bb_err_t m_set_brightness(void *s, uint16_t i, uint8_t p)                       { ((mock_state_t *)s)->pct[i] = p; return BB_OK; }
static bb_err_t m_set_color     (void *s, uint16_t i, uint8_t r, uint8_t g, uint8_t b) { mock_state_t *m = s; m->r[i]=r; m->g[i]=g; m->b[i]=b; return BB_OK; }
static bb_err_t m_flush         (void *s)                                              { ((mock_state_t *)s)->flushed = true; return BB_OK; }
static bb_err_t m_close         (void *s)                                              { ((mock_state_t *)s)->closed  = true; return BB_OK; }

static const bb_led_driver_t mock_full = {
    .set_on=m_set_on, .set_brightness=m_set_brightness, .set_color=m_set_color,
    .flush=m_flush, .close=m_close,
    .caps  = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB,
    .count = 4,
};

static const bb_led_driver_t mock_onoff_only = {
    .set_on=m_set_on, .set_brightness=m_set_brightness, .set_color=m_set_color,
    .flush=m_flush, .close=m_close,
    .caps  = BB_LED_CAP_ONOFF,
    .count = 1,
};

// Reset mock state between tests — called by test_main.c setUp().
void bb_led_test_reset(void) { memset(&g_mock, 0, sizeof g_mock); }

void test_bb_led_caps_and_count(void)
{
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_handle_create(&mock_full, &g_mock, &h));
    TEST_ASSERT_EQUAL(BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB, bb_led_caps(h));
    TEST_ASSERT_EQUAL_UINT16(4, bb_led_count(h));
    bb_led_close(h);
}

void test_bb_led_set_on(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 2, true));
    TEST_ASSERT_TRUE(g_mock.on[2]);
    bb_led_close(h);
}

void test_bb_led_set_color_unsupported_when_no_rgb_cap(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_onoff_only, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_led_set_color(h, 0, 1, 2, 3));
    TEST_ASSERT_EQUAL_UINT8(0, g_mock.r[0]); // driver not called
    bb_led_close(h);
}

void test_bb_led_idx_out_of_range(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_on(h, 99, true));
    bb_led_close(h);
}

void test_bb_led_fill_color_iterates(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_fill_color(h, 0xAA, 0xBB, 0xCC));
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAA, g_mock.r[i]);
        TEST_ASSERT_EQUAL_UINT8(0xBB, g_mock.g[i]);
        TEST_ASSERT_EQUAL_UINT8(0xCC, g_mock.b[i]);
    }
    bb_led_close(h);
}

void test_bb_led_close_calls_driver(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_close(h));
    TEST_ASSERT_TRUE(g_mock.closed);
}

void test_bb_led_brightness_pct_validation(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_brightness(h, 0, 101));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 100));
    TEST_ASSERT_EQUAL_UINT8(100, g_mock.pct[0]);
    bb_led_close(h);
}
