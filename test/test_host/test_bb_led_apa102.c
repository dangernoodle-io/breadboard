// Tests for bb_led_apa102 APA102 RGB strip driver.
#include "unity.h"
#include "bb_led_apa102.h"
#include "bb_led_apa102_host.h"
#include "bb_led_gamma.h"

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

// B1-243: set_level distributes across APA102 5-bit global AND 8-bit color for
// fine resolution at dim levels; hue (green stays green) must be preserved.
// At level16=32768: env=12071, target=1456, g5=6, col=242 → header=0xE6, G=242.
void test_apa102_set_level_scales_color_with_gamma(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 1, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0x00, 0xFF, 0x00));  // green base
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 0, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 32768));             // ~50%
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));

    size_t len;
    const uint8_t *buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL(9, len);  // 4 start + 1 LED*4 + 1 end

    // New algorithm: env=12071, target=12071*7905/65535=1456, g5=ceil(1456/255)=6, col=1456/6=242
    // header=0xE0|6=0xE6, G=255*242/255=242, B=0, R=0 — hue preserved
    TEST_ASSERT_EQUAL_UINT8(0xE6, buf[4]);        // header 0xE0|6 (global=6 in fine path)
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[5]);        // B (green base → 0)
    TEST_ASSERT_EQUAL_UINT8(242, buf[6]);         // G = 255*col/255 = col = 242
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[7]);        // R (green base → 0) — hue preserved
    TEST_ASSERT_TRUE(buf[6] > 0 && buf[6] < 255); // dimmed but non-zero

    bb_led_close(h);
}

// Regression: set_level (and set_brightness) must enable the pixel on their own.
// bb_led_anim drives brightness via set_level and NEVER calls set_on, so a
// missing enable left APA102 boards (e.g. tdongle) dark for the whole boot-solid
// + breathe sequence. The wire header must NOT be the disabled off-frame (0xE0).
// At level16=32768: g5=6, col=242 → header=0xE6 (not 0xE0), R=242.
void test_apa102_set_level_enables_without_set_on(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 1, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0xFF, 0x00, 0x00));  // red base
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 32768));             // ~50%, NO set_on
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));

    size_t len;
    const uint8_t *buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);
    // New algorithm: g5=6, col=242; header=0xE6 (NOT 0xE0 off-frame, NOT 0xFF full)
    TEST_ASSERT_NOT_EQUAL(0xE0, buf[4]);          // enabled — NOT the off-frame
    TEST_ASSERT_EQUAL_UINT8(0xE6, buf[4]);        // enabled at global=6
    TEST_ASSERT_EQUAL_UINT8(242, buf[7]);         // red rendered at col=242
    TEST_ASSERT_TRUE(buf[7] > 0);                 // the LED is lit, not dark

    bb_led_close(h);
}

void test_apa102_set_brightness_enables_without_set_on(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 1, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));

    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0xFF, 0xFF, 0xFF));  // white
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 50));           // 50%, NO set_on
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));

    size_t len;
    const uint8_t *buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT8(0xE0 | 15, buf[4]);   // enabled at bri 15 (50%·31/100) — not the off-frame
    TEST_ASSERT_EQUAL_UINT8(0xFF, buf[5]);        // white rendered (B)

    // brightness 0 turns it back off
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 0));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));
    buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_EQUAL_UINT8(0xE0, buf[4]);        // disabled

    bb_led_close(h);
}

// Resolution win: two distinct low level16 values that previously mapped to the
// same 8-bit color (old_col=0 for both) now produce DIFFERENT emitted frames
// because the new algorithm uses the full global*color product space.
// level16=100: env≈1, target=0, g5=1, col=1 → white pixel color bytes = 1
// level16=160: env≈2, target=0, g5=1, col=2 → white pixel color bytes = 2
// (Both had old_col=0, i.e. the old algorithm emitted identical zero color bytes.)
void test_apa102_level_resolution_improves_at_low_level(void)
{
    bb_led_apa102_cfg_t cfg = { .pin_clk = 10, .pin_din = 11, .led_count = 1, .global_brightness_31 = 31 };
    bb_led_handle_t h;
    size_t len;
    const uint8_t *buf;

    // Render at level16=100
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0xFF, 0xFF, 0xFF));  // white base
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 100));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));
    buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);
    // header=0xE0|1=0xE1, color bytes all=col_a=1
    uint8_t hdr_a   = buf[4];
    uint8_t color_a = buf[7];  // R channel (white → same for all channels)
    bb_led_close(h);

    // Render at level16=160
    TEST_ASSERT_EQUAL(BB_OK, bb_led_apa102_open(&cfg, &h));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_color(h, 0, 0xFF, 0xFF, 0xFF));  // white base
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 160));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_flush(h));
    buf = bb_led_apa102_host_last_buf(0, &len);
    TEST_ASSERT_NOT_NULL(buf);
    uint8_t hdr_b   = buf[4];
    uint8_t color_b = buf[7];  // R channel
    bb_led_close(h);

    // The two frames must differ (monotonic brightness product)
    TEST_ASSERT_TRUE(color_a != color_b || hdr_a != hdr_b);
    // And the higher level must produce a strictly greater or equal product
    uint32_t product_a = (hdr_a & 0x1F) * (uint32_t)color_a;
    uint32_t product_b = (hdr_b & 0x1F) * (uint32_t)color_b;
    TEST_ASSERT_TRUE(product_b >= product_a);
}

void bb_led_apa102_host_test_reset(void) {
    bb_led_apa102_host_reset();
}
