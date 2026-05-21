/* Tests for bb_display core: backend registration, init walk, probe/init
 * dispatch, and blit/clear/off delegation.
 *
 * bb_display_spi_common (SPI init + bounce-buffer blit) is ESP-IDF only
 * and cannot be unit-tested on host without mocking esp_lcd_panel_draw_bitmap.
 * These tests exercise the portable core logic in bb_display.c instead.
 */
#include "unity.h"
#include "bb_display.h"
#include "bb_display_backend.h"
#include "../../components/bb_display/bb_display_test.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Mock backend helpers
 * --------------------------------------------------------------------------- */

static int g_init_calls;
static int g_clear_calls;
static int g_blit_calls;
static int g_off_calls;
static int g_flush_calls;
static int g_draw_text_calls;
static int g_set_rotation_calls;
static int g_set_rotation_result;   /* BB_OK or error */
static int g_probe_result;   /* BB_OK or error */
static bool g_init_succeed;
static uint16_t g_last_clear_color;
static int16_t g_last_blit_x, g_last_blit_y;
static uint16_t g_last_blit_w, g_last_blit_h;

static void reset_mock(void)
{
    g_init_calls    = 0;
    g_clear_calls   = 0;
    g_blit_calls    = 0;
    g_off_calls     = 0;
    g_flush_calls   = 0;
    g_draw_text_calls = 0;
    g_set_rotation_calls = 0;
    g_set_rotation_result = BB_OK;
    g_probe_result  = BB_OK;
    g_init_succeed  = true;
    g_last_clear_color = 0;
    g_last_blit_x = g_last_blit_y = 0;
    g_last_blit_w = g_last_blit_h = 0;
}

static bb_err_t mock_probe(void)
{
    return (bb_err_t)g_probe_result;
}

static bb_err_t mock_init(uint16_t *w, uint16_t *h)
{
    g_init_calls++;
    if (!g_init_succeed) return BB_ERR_INVALID_STATE;
    *w = 320;
    *h = 240;
    return BB_OK;
}

static void mock_clear(uint16_t rgb565)
{
    g_clear_calls++;
    g_last_clear_color = rgb565;
}

static void mock_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    (void)pixels;
    g_blit_calls++;
    g_last_blit_x = x; g_last_blit_y = y;
    g_last_blit_w = w; g_last_blit_h = h;
}

static void mock_off(void)
{
    g_off_calls++;
}

static void mock_flush(void)
{
    g_flush_calls++;
}

static void mock_draw_text(int16_t x, int16_t y, const char *text,
                           const bb_display_font_t *font,
                           uint16_t fg, uint16_t bg)
{
    (void)x; (void)y; (void)text; (void)font; (void)fg; (void)bg;
    g_draw_text_calls++;
}

static bb_err_t mock_set_rotation(uint16_t deg, uint16_t *w, uint16_t *h)
{
    (void)deg;
    g_set_rotation_calls++;
    *w = 320; *h = 240;
    return (bb_err_t)g_set_rotation_result;
}

static const bb_display_backend_t s_mock = {
    .name         = "mock",
    .probe        = NULL,   /* override per-test */
    .init         = mock_init,
    .clear        = mock_clear,
    .blit         = mock_blit,
    .flush        = NULL,
    .off          = mock_off,
    .draw_text    = NULL,
    .set_rotation = NULL,
};

static bb_display_backend_t make_mock(bool with_probe)
{
    bb_display_backend_t b = s_mock;
    b.probe = with_probe ? mock_probe : NULL;
    return b;
}

static bb_display_backend_t make_full_mock(bool with_flush, bool with_draw_text,
                                           bool with_set_rotation)
{
    bb_display_backend_t b = s_mock;
    b.flush        = with_flush        ? mock_flush        : NULL;
    b.draw_text    = with_draw_text    ? mock_draw_text    : NULL;
    b.set_rotation = with_set_rotation ? mock_set_rotation : NULL;
    return b;
}

/* ---------------------------------------------------------------------------
 * Per-test reset — called from test_main.c global setUp.
 * --------------------------------------------------------------------------- */

void bb_display_test_reset_mock(void)
{
    reset_mock();
}

/* ---------------------------------------------------------------------------
 * Tests: registration
 * --------------------------------------------------------------------------- */

void test_bb_display_not_ready_before_init(void)
{
    TEST_ASSERT_FALSE(bb_display_ready());
}

void test_bb_display_init_no_backend_returns_err(void)
{
    bb_err_t err = bb_display_init();
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(bb_display_ready());
}

void test_bb_display_init_with_backend_succeeds(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    TEST_ASSERT_EQUAL(BB_OK, bb_display_init());
    TEST_ASSERT_TRUE(bb_display_ready());
    TEST_ASSERT_EQUAL_UINT16(320, bb_display_width());
    TEST_ASSERT_EQUAL_UINT16(240, bb_display_height());
}

void test_bb_display_init_calls_init_once(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_init();   /* second call: idempotent */
    TEST_ASSERT_EQUAL_INT(1, g_init_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: probe walk
 * --------------------------------------------------------------------------- */

void test_bb_display_probe_ok_proceeds_to_init(void)
{
    g_probe_result = BB_OK;
    bb_display_backend_t b = make_mock(true);
    bb_display_register_backend(&b);
    TEST_ASSERT_EQUAL(BB_OK, bb_display_init());
    TEST_ASSERT_EQUAL_INT(1, g_init_calls);
}

void test_bb_display_probe_fail_skips_init(void)
{
    g_probe_result = BB_ERR_NOT_FOUND;
    bb_display_backend_t b = make_mock(true);
    bb_display_register_backend(&b);
    bb_err_t err = bb_display_init();
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, g_init_calls);
}

static int s_second_init_calls = 0;

static bb_err_t second_backend_init(uint16_t *w, uint16_t *h)
{
    s_second_init_calls++;
    *w = 128; *h = 64;
    return BB_OK;
}

void test_bb_display_falls_through_to_second_backend(void)
{
    /* First backend: probe fails. Second: no probe, init succeeds. */
    g_probe_result = BB_ERR_NOT_FOUND;
    s_second_init_calls = 0;
    bb_display_backend_t b1 = make_mock(true);

    bb_display_backend_t b2 = {
        .name  = "second",
        .probe = NULL,
        .init  = second_backend_init,
        .clear = mock_clear,
        .blit  = mock_blit,
        .off   = mock_off,
    };

    bb_display_register_backend(&b1);
    bb_display_register_backend(&b2);
    TEST_ASSERT_EQUAL(BB_OK, bb_display_init());
    TEST_ASSERT_EQUAL_INT(0, g_init_calls);       /* b1 skipped */
    TEST_ASSERT_EQUAL_INT(1, s_second_init_calls);
    TEST_ASSERT_EQUAL_UINT16(128, bb_display_width());
    TEST_ASSERT_EQUAL_UINT16(64, bb_display_height());
}

/* ---------------------------------------------------------------------------
 * Tests: init fail
 * --------------------------------------------------------------------------- */

void test_bb_display_init_fail_returns_err(void)
{
    g_init_succeed = false;
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_display_init());
    TEST_ASSERT_FALSE(bb_display_ready());
}

/* ---------------------------------------------------------------------------
 * Tests: dispatch after init
 * --------------------------------------------------------------------------- */

void test_bb_display_clear_dispatches(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_clear(0xF800);
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
    TEST_ASSERT_EQUAL_UINT16(0xF800, g_last_clear_color);
}

void test_bb_display_blit_dispatches(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    uint16_t px[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    bb_display_blit(10, 20, 2, 2, px);
    TEST_ASSERT_EQUAL_INT(1, g_blit_calls);
    TEST_ASSERT_EQUAL_INT16(10, g_last_blit_x);
    TEST_ASSERT_EQUAL_INT16(20, g_last_blit_y);
    TEST_ASSERT_EQUAL_UINT16(2, g_last_blit_w);
    TEST_ASSERT_EQUAL_UINT16(2, g_last_blit_h);
}

void test_bb_display_blit_null_pixels_no_dispatch(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_blit(0, 0, 10, 10, NULL);
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

void test_bb_display_blit_zero_dimensions_no_dispatch(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    uint16_t px = 0;
    bb_display_blit(0, 0, 0, 10, &px);
    bb_display_blit(0, 0, 10, 0, &px);
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

void test_bb_display_off_dispatches(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_off();
    TEST_ASSERT_EQUAL_INT(1, g_off_calls);
    TEST_ASSERT_FALSE(bb_display_ready());
}

void test_bb_display_no_dispatch_before_init(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    /* Do NOT call bb_display_init() */
    bb_display_clear(0xFFFF);
    uint16_t px = 0;
    bb_display_blit(0, 0, 1, 1, &px);
    TEST_ASSERT_EQUAL_INT(0, g_clear_calls);
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: set_rotation
 * --------------------------------------------------------------------------- */

void test_bb_display_set_rotation_no_backend_returns_err(void)
{
    /* Not inited */
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_display_set_rotation(90));
}

void test_bb_display_set_rotation_invalid_angle(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_display_set_rotation(45));
}

void test_bb_display_set_rotation_no_support_returns_err(void)
{
    bb_display_backend_t b = make_mock(false);
    /* set_rotation is NULL in s_mock */
    bb_display_register_backend(&b);
    bb_display_init();
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_display_set_rotation(90));
}

/* ---------------------------------------------------------------------------
 * Tests: blit byte-swap correctness (pure math, no hardware)
 *
 * The RGB565 byte-swap expression used in bb_display_spi_common is:
 *   out = (c >> 8) | (c << 8)
 * Verify it matches (c & 0xFF) << 8 | (c >> 8) & 0xFF (the st77xx form).
 * --------------------------------------------------------------------------- */

void test_rgb565_byteswap_forms_equivalent(void)
{
    uint16_t samples[] = {0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xABCD, 0x1234};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint16_t c = samples[i];
        uint16_t form1 = (uint16_t)((c >> 8) | (c << 8));
        uint16_t form2 = (uint16_t)(((c & 0xFF) << 8) | ((c >> 8) & 0xFF));
        TEST_ASSERT_EQUAL_UINT16(form2, form1);
    }
}

void test_rgb565_byteswap_round_trip(void)
{
    uint16_t samples[] = {0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xBEEF};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint16_t c = samples[i];
        uint16_t swapped = (uint16_t)((c >> 8) | (c << 8));
        uint16_t restored = (uint16_t)((swapped >> 8) | (swapped << 8));
        TEST_ASSERT_EQUAL_UINT16(c, restored);
    }
}

/* ---------------------------------------------------------------------------
 * Tests: bounce-buffer row-pass math
 *
 * Given BOUNCE_PIXELS=512, verify rows_this_pass and pixels_this_pass
 * calculations are correct for various widths and remaining row counts.
 * --------------------------------------------------------------------------- */

#define BOUNCE_PIXELS 512u

static size_t calc_rows_this_pass(size_t w, size_t h_remaining)
{
    size_t rows = BOUNCE_PIXELS / w;
    if (rows == 0) rows = 1;
    if (h_remaining < rows) rows = h_remaining;
    return rows;
}

static size_t calc_pixels_this_pass(size_t w, size_t rows)
{
    size_t px = rows * w;
    if (px > BOUNCE_PIXELS) px = BOUNCE_PIXELS;
    return px;
}

void test_bounce_rows_narrow_image(void)
{
    /* w=1: should be 512 rows per pass (capped by height). */
    size_t rows = calc_rows_this_pass(1, 100);
    TEST_ASSERT_EQUAL_UINT(100, rows);   /* h_remaining < 512 */
    size_t px = calc_pixels_this_pass(1, rows);
    TEST_ASSERT_EQUAL_UINT(100, px);
}

void test_bounce_rows_wide_image(void)
{
    /* w=512: one row per pass. */
    size_t rows = calc_rows_this_pass(512, 10);
    TEST_ASSERT_EQUAL_UINT(1, rows);
    size_t px = calc_pixels_this_pass(512, rows);
    TEST_ASSERT_EQUAL_UINT(512, px);
}

void test_bounce_rows_wider_than_bounce(void)
{
    /* w=1000 > 512: still one row, capped to BOUNCE_PIXELS pixels. */
    size_t rows = calc_rows_this_pass(1000, 5);
    TEST_ASSERT_EQUAL_UINT(1, rows);    /* 512/1000 = 0 → forced to 1 */
    size_t px = calc_pixels_this_pass(1000, rows);
    TEST_ASSERT_EQUAL_UINT(BOUNCE_PIXELS, px);  /* capped */
}

void test_bounce_rows_exact_fit(void)
{
    /* w=256: 2 rows per pass, 4 rows total → 2 full passes. */
    size_t rows1 = calc_rows_this_pass(256, 4);
    TEST_ASSERT_EQUAL_UINT(2, rows1);
    size_t rows2 = calc_rows_this_pass(256, 2);
    TEST_ASSERT_EQUAL_UINT(2, rows2);
    /* Next pass (0 remaining) would not be called — boundary check only. */
}

void test_bounce_rows_partial_last_pass(void)
{
    /* w=100, remaining=1: one row. */
    size_t rows = calc_rows_this_pass(100, 1);
    TEST_ASSERT_EQUAL_UINT(1, rows);
    size_t px = calc_pixels_this_pass(100, rows);
    TEST_ASSERT_EQUAL_UINT(100, px);
}

/* ---------------------------------------------------------------------------
 * Tests: flush
 * --------------------------------------------------------------------------- */

void test_bb_display_flush_dispatches(void)
{
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_flush();
    TEST_ASSERT_EQUAL_INT(1, g_flush_calls);
}

void test_bb_display_flush_no_flush_fn_is_noop(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_flush();   /* flush is NULL in s_mock */
    TEST_ASSERT_EQUAL_INT(0, g_flush_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: set_rotation happy path
 * --------------------------------------------------------------------------- */

void test_bb_display_set_rotation_succeeds(void)
{
    bb_display_backend_t b = make_full_mock(false, false, true);
    bb_display_register_backend(&b);
    bb_display_init();
    TEST_ASSERT_EQUAL(BB_OK, bb_display_set_rotation(90));
    TEST_ASSERT_EQUAL_INT(1, g_set_rotation_calls);
}

void test_bb_display_set_rotation_updates_dimensions(void)
{
    bb_display_backend_t b = make_full_mock(false, false, true);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_set_rotation(90);
    /* mock_set_rotation sets w=320, h=240 regardless of angle */
    TEST_ASSERT_EQUAL_UINT16(320, bb_display_width());
    TEST_ASSERT_EQUAL_UINT16(240, bb_display_height());
}

void test_bb_display_set_rotation_propagates_error(void)
{
    g_set_rotation_result = BB_ERR_INVALID_STATE;
    bb_display_backend_t b = make_full_mock(false, false, true);
    bb_display_register_backend(&b);
    bb_display_init();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_display_set_rotation(90));
}

/* ---------------------------------------------------------------------------
 * Tests: backend registry edge cases
 * --------------------------------------------------------------------------- */

void test_bb_display_register_null_is_ignored(void)
{
    bb_display_register_backend(NULL);
    /* No crash and still no backend. */
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_display_init());
}

void test_bb_display_backend_with_null_init_is_skipped(void)
{
    bb_display_backend_t b = make_mock(false);
    b.init = NULL;
    bb_display_register_backend(&b);
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_display_init());
}

void test_bb_display_registry_full_drops_excess(void)
{
    /* BB_DISPLAY_MAX_BACKENDS == 8; fill 8 slots, 9th is silently dropped. */
    bb_display_backend_t backends[9];
    for (int i = 0; i < 9; i++) {
        backends[i] = make_mock(false);
    }
    for (int i = 0; i < 9; i++) {
        bb_display_register_backend(&backends[i]);
    }
    /* Init should still work (first 8 are registered). */
    TEST_ASSERT_EQUAL(BB_OK, bb_display_init());
}

void test_bb_display_init_idempotent_after_success(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    TEST_ASSERT_EQUAL(BB_OK, bb_display_init());
    TEST_ASSERT_EQUAL(BB_OK, bb_display_init());  /* second call returns BB_OK */
    TEST_ASSERT_EQUAL_INT(1, g_init_calls);       /* init not called twice */
}

/* ---------------------------------------------------------------------------
 * Tests: draw_text dispatch
 * --------------------------------------------------------------------------- */

void test_bb_display_draw_text_before_init_noop(void)
{
    bb_display_backend_t b = make_full_mock(false, true, false);
    bb_display_register_backend(&b);
    bb_display_draw_text(0, 0, "hi", NULL, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(0, g_draw_text_calls);
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

void test_bb_display_draw_text_null_text_noop(void)
{
    bb_display_backend_t b = make_full_mock(false, true, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_draw_text(0, 0, NULL, &bb_display_font_8x16, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(0, g_draw_text_calls);
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

void test_bb_display_draw_text_uses_backend_draw_text(void)
{
    bb_display_backend_t b = make_full_mock(false, true, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_draw_text(0, 0, "hi", &bb_display_font_8x16, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(1, g_draw_text_calls);
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);  /* backend draw_text used, not fallback blit */
}

void test_bb_display_draw_text_falls_back_to_blit(void)
{
    /* Backend has no draw_text: falls back to rasterize + blit. */
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_draw_text(0, 0, "AB", &bb_display_font_8x16, 0xFFFF, 0x0000);
    /* 2 chars → 2 blit calls. */
    TEST_ASSERT_EQUAL_INT(2, g_blit_calls);
}

void test_bb_display_draw_text_null_font_uses_default(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    /* No explicit font — uses compile-time default (8x16). */
    bb_display_draw_text(0, 0, "A", NULL, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(1, g_blit_calls);
}

void test_bb_display_set_default_font_null_restores_default(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    /* Override then restore. */
    bb_display_set_default_font(&bb_display_font_5x8);
    bb_display_set_default_font(NULL);
    bb_display_draw_text(0, 0, "A", NULL, 0xFFFF, 0x0000);
    /* After restoring default, draw_text should still work (blit once for 'A'). */
    TEST_ASSERT_EQUAL_INT(1, g_blit_calls);
}

void test_bb_display_set_default_font_custom(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_set_default_font(&bb_display_font_5x8);
    bb_display_draw_text(0, 0, "AB", NULL, 0xFFFF, 0x0000);
    /* 2 chars with custom font → 2 blit calls. */
    TEST_ASSERT_EQUAL_INT(2, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: show_splash / show_prov
 * --------------------------------------------------------------------------- */

void test_bb_display_show_splash_before_init_noop(void)
{
    bb_display_show_splash("Hello", "v1.0", &bb_display_font_8x16);
    TEST_ASSERT_EQUAL_INT(0, g_clear_calls);
}

void test_bb_display_show_splash_calls_clear_and_blit(void)
{
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_show_splash("Hello", "v1.0", &bb_display_font_8x16);
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
    TEST_ASSERT_TRUE(g_blit_calls > 0);
    TEST_ASSERT_EQUAL_INT(1, g_flush_calls);  /* flush called at end of render_centered_lines */
}

void test_bb_display_show_prov_before_init_noop(void)
{
    bb_display_show_prov("ssid", "pass", &bb_display_font_8x16);
    TEST_ASSERT_EQUAL_INT(0, g_clear_calls);
}

void test_bb_display_show_prov_calls_clear_and_blit(void)
{
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_show_prov("ssid", "pass", &bb_display_font_8x16);
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
    TEST_ASSERT_TRUE(g_blit_calls > 0);
}

void test_bb_display_show_splash_null_strings(void)
{
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    /* NULL product/version → uses "" — must not crash. */
    bb_display_show_splash(NULL, NULL, &bb_display_font_8x16);
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
}

void test_bb_display_show_prov_null_strings(void)
{
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_show_prov(NULL, NULL, &bb_display_font_8x16);
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: off when no off fn
 * --------------------------------------------------------------------------- */

void test_bb_display_off_with_no_off_fn(void)
{
    bb_display_backend_t b = make_mock(false);
    b.off = NULL;
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_off();  /* must not crash */
    TEST_ASSERT_FALSE(bb_display_ready());
}

/* ---------------------------------------------------------------------------
 * Tests: draw_text with no font at all (both arg and default NULL)
 * --------------------------------------------------------------------------- */

void test_bb_display_draw_text_no_font_at_all_noop(void)
{
    /* Force default font to NULL so draw_text(font=NULL) falls into the
     * "no font available" path and returns without blitting. */
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    /* Override default font away, then set it to NULL explicitly to clear it. */
    bb_display_set_default_font(&bb_display_font_5x8);   /* set non-null first */
    bb_display_set_default_font(NULL);                    /* restore compile-time default (8x16) */
    /* There is no way to null out the compile-time default without a custom build,
     * so test the NULL-text guard path instead. */
    bb_display_draw_text(0, 0, NULL, NULL, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: render_centered_lines no-font path
 * --------------------------------------------------------------------------- */

void test_bb_display_show_splash_null_font_uses_default(void)
{
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    /* NULL font → uses compile-time default. Must not crash. */
    bb_display_show_splash("Hi", "v1", NULL);
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
    TEST_ASSERT_TRUE(g_blit_calls > 0);
}

void test_bb_display_show_prov_null_font_uses_default(void)
{
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_show_prov("ap", "pw", NULL);
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
    TEST_ASSERT_TRUE(g_blit_calls > 0);
}

/* ---------------------------------------------------------------------------
 * Tests: NULL clear/blit fn in backend
 * --------------------------------------------------------------------------- */

void test_bb_display_clear_null_fn_is_noop(void)
{
    bb_display_backend_t b = make_mock(false);
    b.clear = NULL;
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_clear(0xFFFF);  /* clear fn is NULL → no-op */
    TEST_ASSERT_EQUAL_INT(0, g_clear_calls);
}

void test_bb_display_blit_null_fn_is_noop(void)
{
    bb_display_backend_t b = make_mock(false);
    b.blit = NULL;
    bb_display_register_backend(&b);
    bb_display_init();
    uint16_t px = 0xFFFF;
    bb_display_blit(0, 0, 1, 1, &px);  /* blit fn is NULL → no-op */
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: backend with NULL name (logs "?" fallback)
 * --------------------------------------------------------------------------- */

void test_bb_display_null_name_backend_inits(void)
{
    bb_display_backend_t b = make_mock(false);
    b.name = NULL;
    bb_display_register_backend(&b);
    TEST_ASSERT_EQUAL(BB_OK, bb_display_init());
    TEST_ASSERT_TRUE(bb_display_ready());
}

/* ---------------------------------------------------------------------------
 * Tests: default font already set before init (the else branch of s_default_font==NULL)
 * --------------------------------------------------------------------------- */

void test_bb_display_default_font_set_before_init_preserved(void)
{
    bb_display_set_default_font(&bb_display_font_5x8);
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    /* Font was set before init; init must not clobber it.
     * Verify by drawing with NULL font arg → blit must fire (uses pre-set font). */
    bb_display_draw_text(0, 0, "X", NULL, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(1, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: rasterize_glyph out-of-range codepoint (renders blank glyph)
 * --------------------------------------------------------------------------- */

void test_bb_display_draw_text_out_of_range_codepoint(void)
{
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    /* bb_display_font_8x16 covers ASCII 32-127. Pass char 1 (out of range)
     * → rasterize_glyph produces an all-bg glyph but must not crash. */
    char buf[2] = {1, 0};
    bb_display_draw_text(0, 0, buf, &bb_display_font_8x16, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(1, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: draw_text with oversized font (> BB_DISPLAY_RASTER_MAX_W/H)
 * --------------------------------------------------------------------------- */

void test_bb_display_draw_text_oversized_font_noop(void)
{
    static const uint8_t dummy_bitmap[1] = {0};
    bb_display_font_t big = {
        .glyph_w = 33,   /* > BB_DISPLAY_RASTER_MAX_W = 32 */
        .glyph_h = 1,
        .first_codepoint = 32,
        .glyph_count = 1,
        .bitmap = dummy_bitmap,
    };
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_draw_text(0, 0, " ", &big, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: draw_text with NULL blit fn (falls into no-blit path)
 * --------------------------------------------------------------------------- */

void test_bb_display_draw_text_null_blit_fn_noop(void)
{
    bb_display_backend_t b = make_mock(false);
    b.blit = NULL;
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_draw_text(0, 0, "A", &bb_display_font_8x16, 0xFFFF, 0x0000);
    /* blit is NULL so rasterizer path is skipped */
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: render_centered_lines with y0 < 0 clamped to 0
 * --------------------------------------------------------------------------- */

void test_bb_display_show_splash_tall_font_clamps_y(void)
{
    /* Use a font taller than the mock display height (240) to force y0 < 0 */
    static const uint8_t dummy_bitmap[2] = {0, 0};
    bb_display_font_t tall = {
        .glyph_w = 8,
        .glyph_h = 200,   /* 2 lines * 200 = 400 > 240 → y0 < 0 */
        .first_codepoint = 32,
        .glyph_count = 1,
        .bitmap = dummy_bitmap,
    };
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_show_splash("Hi", "v2", &tall);
    /* y0 clamped to 0 — must not crash */
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: render_centered_lines with x < 0 clamped to 0
 * --------------------------------------------------------------------------- */

void test_bb_display_show_splash_wide_text_clamps_x(void)
{
    /* Text wider than display width (320) → x < 0, clamped. */
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    /* 8x16 font, display width 320: "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
     * 41 chars * 8 pixels = 328 > 320 → x would be negative, gets clamped to 0. */
    bb_display_show_splash("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", "v1",
                           &bb_display_font_8x16);
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
    TEST_ASSERT_TRUE(g_blit_calls > 0);
}

/* ---------------------------------------------------------------------------
 * Tests: render_centered_lines with NULL line entry (uses "")
 * --------------------------------------------------------------------------- */

void test_bb_display_show_splash_null_line_ok(void)
{
    /* show_splash passes product/version through lines[] — NULL handled as "". */
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_show_splash(NULL, NULL, &bb_display_font_8x16);
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: flush / off before init (cover the !s_ready early-return branches)
 * --------------------------------------------------------------------------- */

void test_bb_display_flush_before_ready_is_noop(void)
{
    /* flush before init: must not crash */
    bb_display_flush();
    TEST_ASSERT_EQUAL_INT(0, g_flush_calls);
}

void test_bb_display_off_before_ready_is_noop(void)
{
    /* off before init: must not crash */
    bb_display_off();
    TEST_ASSERT_FALSE(bb_display_ready());
}

/* ---------------------------------------------------------------------------
 * Tests: registry-full log with NULL-name backend
 * --------------------------------------------------------------------------- */

void test_bb_display_registry_full_null_name_dropped(void)
{
    /* Fill registry with 8 valid backends then try a 9th with NULL name.
     * Exercises the null-name ternary in the registry-full log path. */
    bb_display_backend_t backends[9];
    for (int i = 0; i < 8; i++) {
        backends[i] = make_mock(false);
    }
    backends[8] = make_mock(false);
    backends[8].name = NULL;
    for (int i = 0; i < 8; i++) {
        bb_display_register_backend(&backends[i]);
    }
    bb_display_register_backend(&backends[8]);  /* 9th: null name, must not crash */
    TEST_ASSERT_EQUAL(BB_OK, bb_display_init());  /* first 8 still registered */
}

/* ---------------------------------------------------------------------------
 * Tests: init fail + probe fail with NULL-name backend
 * --------------------------------------------------------------------------- */

void test_bb_display_init_fail_null_name(void)
{
    /* Backend with NULL name that fails init: covers null-name in error log. */
    g_init_succeed = false;
    bb_display_backend_t b = make_mock(false);
    b.name = NULL;
    bb_display_register_backend(&b);
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_display_init());
}

void test_bb_display_probe_fail_null_name(void)
{
    /* Backend with NULL name whose probe fails: covers null-name in probe-fail log. */
    g_probe_result = BB_ERR_NOT_FOUND;
    bb_display_backend_t b = make_mock(true);
    b.name = NULL;
    bb_display_register_backend(&b);
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_display_init());
}

/* ---------------------------------------------------------------------------
 * Tests: probe succeeds — exercises the probed=1 log path (probed=yes)
 * --------------------------------------------------------------------------- */

void test_bb_display_probe_success_logs_probed(void)
{
    /* Backend with probe that returns OK: probed=true in success log. */
    g_probe_result = BB_OK;
    bb_display_backend_t b = make_mock(true);
    bb_display_register_backend(&b);
    TEST_ASSERT_EQUAL(BB_OK, bb_display_init());
    TEST_ASSERT_EQUAL_INT(1, g_init_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: rasterize_glyph — cp in range but past glyph_count (rows stays NULL)
 * --------------------------------------------------------------------------- */

void test_bb_display_draw_text_cp_past_glyph_count(void)
{
    /* Use a font with glyph_count=1 starting at first_codepoint=32 (space).
     * Pass char 33 ('!'): 33 >= 32 (true) but 33-32=1 >= 1 (false) → rows=NULL.
     * This exercises the second && condition being false in rasterize_glyph. */
    static const uint8_t dummy_bitmap[1] = {0xFF};
    bb_display_font_t tiny = {
        .glyph_w         = 4,
        .glyph_h         = 1,
        .first_codepoint = 32,
        .glyph_count     = 1,   /* only space (32) is valid */
        .bitmap          = dummy_bitmap,
    };
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    /* '!' = codepoint 33: first condition passes, second fails → blank glyph */
    bb_display_draw_text(0, 0, "!", &tiny, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(1, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: draw_text / show_splash with no default font at all
 * --------------------------------------------------------------------------- */

void test_bb_display_draw_text_no_default_font_noop(void)
{
    /* After init, force default font to NULL; draw_text with NULL font arg
     * must hit the !font guard and return without blitting. */
    bb_display_backend_t b = make_mock(false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_clear_default_font_for_testing();
    bb_display_draw_text(0, 0, "X", NULL, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

void test_bb_display_show_splash_no_default_font_noop(void)
{
    /* After init, force default font to NULL; show_splash with NULL font arg
     * must hit the !font guard in render_centered_lines and return. */
    bb_display_backend_t b = make_full_mock(true, false, false);
    bb_display_register_backend(&b);
    bb_display_init();
    bb_display_clear_default_font_for_testing();
    bb_display_show_splash("Hi", "v1", NULL);
    /* clear is called (in show_splash before render_centered_lines) */
    TEST_ASSERT_EQUAL_INT(1, g_clear_calls);
    /* but no blit occurs because the font guard exits render_centered_lines */
    TEST_ASSERT_EQUAL_INT(0, g_blit_calls);
}

/* ---------------------------------------------------------------------------
 * Tests: set_rotation with all four valid angles (exercises the && chain)
 * --------------------------------------------------------------------------- */

void test_bb_display_set_rotation_zero(void)
{
    bb_display_backend_t b = make_full_mock(false, false, true);
    bb_display_register_backend(&b);
    bb_display_init();
    TEST_ASSERT_EQUAL(BB_OK, bb_display_set_rotation(0));
}

void test_bb_display_set_rotation_180(void)
{
    bb_display_backend_t b = make_full_mock(false, false, true);
    bb_display_register_backend(&b);
    bb_display_init();
    TEST_ASSERT_EQUAL(BB_OK, bb_display_set_rotation(180));
}

void test_bb_display_set_rotation_270(void)
{
    bb_display_backend_t b = make_full_mock(false, false, true);
    bb_display_register_backend(&b);
    bb_display_init();
    TEST_ASSERT_EQUAL(BB_OK, bb_display_set_rotation(270));
}
