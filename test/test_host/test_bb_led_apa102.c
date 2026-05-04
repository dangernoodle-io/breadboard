// Tests for bb_led_apa102 APA102 RGB strip driver.
#include "unity.h"
#include "bb_led_apa102.h"
#include "bb_led_apa102_host.h"

void test_apa102_open_close(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 4, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB, bb_led_caps(h));
    TEST_ASSERT_EQUAL_UINT16(4, bb_led_count(h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_close(h));
}

void test_apa102_initial_flush_dark(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 4, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));

    size_t len;
    const uint8_t *buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);

    // Expected: 4 start bytes (0x00), 4 LEDs * 4 bytes (header + BGR), (4+15)/16=1 end byte (0xFF)
    // = 4 + 16 + 1 = 21 bytes
    TEST_ASSERT_EQUAL(21, len);

    // Check start frame.
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[3]);

    // Check per-LED frames: enabled=false, so header=0xE0, RGB all 0.
    for (int i = 0; i < 4; i++) {
        int base = 4 + i * 4;
        TEST_ASSERT_EQUAL_UINT8(0xE0, buf[base]);      // header: 0xE0 | 0 (disabled)
        TEST_ASSERT_EQUAL_UINT8(0x00, buf[base + 1]); // B
        TEST_ASSERT_EQUAL_UINT8(0x00, buf[base + 2]); // G
        TEST_ASSERT_EQUAL_UINT8(0x00, buf[base + 3]); // R
    }

    // Check end frame.
    TEST_ASSERT_EQUAL_UINT8(0xFF, buf[20]);

    bb_led_close(h);
}

void test_apa102_set_color_and_flush(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 1, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0xFF, 0x00, 0x00));  // Red
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));

    size_t len;
    const uint8_t *buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);

    // Expected: 4 start + 4 LED frame + 1 end = 9 bytes
    TEST_ASSERT_EQUAL(9, len);

    // Per-LED frame at index 4: enabled=true, brightness=31
    // Header = 0xE0 | 31 = 0xFF
    TEST_ASSERT_EQUAL_UINT8(0xFF, buf[4]);   // brightness header
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[5]);   // B (0)
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[6]);   // G (0)
    TEST_ASSERT_EQUAL_UINT8(0xFF, buf[7]);   // R (0xFF)

    bb_led_close(h);
}

void test_apa102_set_brightness_partial(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 1, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0xFF, 0xFF, 0xFF));  // White
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 50));            // 50% brightness
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));

    size_t len;
    const uint8_t *buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);

    // Brightness 50% -> (50 * 31) / 100 = 15
    uint8_t expected_header = 0xE0 | 15;  // 0xEF
    TEST_ASSERT_EQUAL_UINT8(expected_header, buf[4]);

    bb_led_close(h);
}

void test_apa102_fill_color(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 3, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_fill_color(h, 0xAA, 0xBB, 0xCC));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 1, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 2, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));

    size_t len;
    const uint8_t *buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);

    // 4 start + 3*4 LED + 1 end = 17 bytes
    TEST_ASSERT_EQUAL(17, len);

    // Check each LED: R=0xAA, G=0xBB, B=0xCC
    for (int i = 0; i < 3; i++) {
        int base = 4 + i * 4;
        TEST_ASSERT_EQUAL_UINT8(0xFF, buf[base]);      // header
        TEST_ASSERT_EQUAL_UINT8(0xCC, buf[base + 1]); // B
        TEST_ASSERT_EQUAL_UINT8(0xBB, buf[base + 2]); // G
        TEST_ASSERT_EQUAL_UINT8(0xAA, buf[base + 3]); // R
    }

    bb_led_close(h);
}

void test_apa102_idx_out_of_range(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 2, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_color(h, 99, 0xFF, 0, 0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_on(h, 2, true));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_brightness(h, 5, 50));

    bb_led_close(h);
}

void test_apa102_invalid_args(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 1, .global_brightness_31 = 31 };
    bb_led_handle_t h;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_apa102_open(NULL, &h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_apa102_open(&cfg, NULL));

    cfg.pin_clk = -1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_apa102_open(&cfg, &h));

    cfg.pin_clk = 10;
    cfg.pin_din = -1;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_apa102_open(&cfg, &h));

    cfg.pin_din = 11;
    cfg.led_count = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_apa102_open(&cfg, &h));

    cfg.led_count = 1;
    cfg.global_brightness_31 = 32;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_apa102_open(&cfg, &h));
}

void test_apa102_disabled_pixel_zeros_rgb(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 1, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0xFF, 0xFF, 0xFF));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, false));  // Disabled
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));

    size_t len;
    const uint8_t *buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);

    // Disabled LED: header=0xE0, RGB all 0
    TEST_ASSERT_EQUAL_UINT8(0xE0, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[6]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[7]);

    bb_led_close(h);
}

void bb_led_apa102_host_test_reset(void) {
    bb_led_apa102_host_reset();
}
