// Portable smoke app — exercises bb_log, bb_nv, bb_wifi, bb_http on every
// supported framework/backend. Entry shims (entry_arduino.cpp / entry_espidf.c)
// call into smoke_app_setup() and smoke_app_loop().
//
// The same source compiles unchanged across all envs in platformio.ini.
// Backend selection happens via build_src_filter and build_flags.

#include "bb_log.h"
#include "bb_nv.h"
#include "bb_http.h"
#include "bb_wifi.h"
#include "bb_led.h"
#include "bb_led_gpio.h"
#include "bb_led_pwm.h"
#include "bb_led_apa102.h"
#include "smoke_app.h"

#if defined(BB_SMOKE_DISPLAY) || defined(BB_WIFI_BACKEND_R4)
#include "bb_display.h"
#endif

static const char *TAG = "smoke";

static bb_err_t ping_handler(bb_http_request_t *req) {
    bb_http_resp_set_header(req, "Content-Type", "text/plain");
    bb_http_resp_send(req, "pong\n", 5);
    bb_log_i(TAG, "GET /ping -> pong");
    return BB_OK;
}

#if defined(BB_WIFI_BACKEND_R4) && defined(BB_DISPLAY_FONT_5X8) && defined(BB_DISPLAY_FONT_6X12)
/* Tiny user-supplied font: 4x4 digits 0..3 only. Demonstrates that consumers
 * can ship their own bb_display_font_t — no rebuild of bb_display required. */
static const uint8_t s_my_glyphs_4x4[4 * 4] = {
    /* '0' */ 0xF0, 0x90, 0x90, 0xF0,
    /* '1' */ 0x60, 0x20, 0x20, 0x70,
    /* '2' */ 0xF0, 0x10, 0x60, 0xF0,
    /* '3' */ 0xF0, 0x70, 0x10, 0xF0,
};
static const bb_display_font_t s_my_font_4x4 = {
    .glyph_w = 4,
    .glyph_h = 4,
    .first_codepoint = 0x30,
    .glyph_count = 4,
    .bitmap = s_my_glyphs_4x4,
};
#endif

void smoke_app_setup(void) {
#if defined(BB_SMOKE_DISPLAY) || defined(BB_WIFI_BACKEND_R4)
    if (bb_display_init() == BB_OK) {
        bb_log_i(TAG, "display: %ux%u", bb_display_width(), bb_display_height());
        bb_display_show_splash("smoke", "boot", NULL);
    } else {
        bb_log_w(TAG, "display: no probe match");
    }
#endif

#if defined(BB_WIFI_BACKEND_R4) && defined(BB_DISPLAY_FONT_5X8) && defined(BB_DISPLAY_FONT_6X12)
    /* Bench showcase: cycle the SSD1315 OLED through every font + rotation
     * combo the bb_display API can express. Runs forever — WiFi/HTTP setup
     * below never fires when this is reached. Disable by undefining one of
     * the BB_DISPLAY_FONT_* knobs to drop into the normal smoke path.
     *
     * The bench OLED is mounted with its native top-of-screen at the bottom
     * of the user's view, so we start at rotation 180 (looks right-side-up
     * to the user) and flip to 0 (upside-down to the user) for each font. */
    bb_log_i(TAG, "display showcase active — wifi/http disabled");

    /* Per-entry labels: line1 = font name, line2 = "size". The 4x4 entry only
     * has digit glyphs 0..3, so its labels are pure digits. */
    struct {
        const char *line1;
        const char *line2;
        const bb_display_font_t *font;
    } fonts[] = {
        { "font", "8x16", NULL                 },  /* NULL → compile-time default 8x16 */
        { "font", "6x12",  &bb_display_font_6x12 },
        { "font", "5x8",  &bb_display_font_5x8 },
        { "0123", "0123", &s_my_font_4x4       },  /* user-supplied; digits 0..3 only */
    };
    const size_t n_fonts = sizeof(fonts) / sizeof(fonts[0]);
    const uint16_t orientations[2] = { 180, 0 };  /* upright, then upside-down */

    for (;;) {
        for (size_t i = 0; i < n_fonts; i++) {
            bb_display_set_default_font(fonts[i].font);
            for (size_t j = 0; j < 2; j++) {
                /* Clear in the CURRENT orientation first so the rotation
                 * flips an already-black screen — eliminates the brief
                 * flash of old-content-mirrored that happens when you
                 * rotate first then clear. */
                bb_display_clear(0x0000);
                bb_display_set_rotation(orientations[j]);
                bb_display_show_splash(fonts[i].line1, fonts[i].line2, NULL);
                bb_log_i(TAG, "showcase: %s @ %u", fonts[i].line2,
                         (unsigned)orientations[j]);
                delay(2000);
            }
        }
    }
#endif
    bb_nv_config_init();
    bb_log_i(TAG, "boot");

    // === bb_led ===
    bb_led_gpio_cfg_t led_cfg = {
        .gpio = 2,
        .active_low = false,
    };
    bb_led_handle_t led_handle = NULL;
    if (bb_led_gpio_open(&led_cfg, &led_handle) == BB_OK) {
        bb_led_set_on(led_handle, 0, false);
        bb_led_close(led_handle);
        bb_log_i(TAG, "bb_led_gpio: ok");
    } else {
        bb_log_w(TAG, "bb_led_gpio: open failed");
    }

    // === bb_led_pwm ===
    bb_led_pwm_cfg_t led_pwm_cfg = {
        .gpio = 4,
        .freq_hz = 5000,
        .resolution_bits = 8,
        .active_low = false,
    };
    bb_led_handle_t led_pwm_handle = NULL;
    if (bb_led_pwm_open(&led_pwm_cfg, &led_pwm_handle) == BB_OK) {
        bb_led_set_brightness(led_pwm_handle, 0, 25);
        bb_led_close(led_pwm_handle);
        bb_log_i(TAG, "bb_led_pwm: ok");
    } else {
        bb_log_w(TAG, "bb_led_pwm: open failed");
    }

    // === bb_led_apa102 ===
    bb_led_apa102_cfg_t led_apa_cfg = {
        .pin_clk = 6,
        .pin_din = 7,
        .led_count = 1,
        .global_brightness_31 = 15,
    };
    bb_led_handle_t led_apa_handle = NULL;
    if (bb_led_apa102_open(&led_apa_cfg, &led_apa_handle) == BB_OK) {
        bb_led_set_color(led_apa_handle, 0, 0, 0, 32);
        bb_led_set_on(led_apa_handle, 0, true);
        bb_led_flush(led_apa_handle);
        bb_led_close(led_apa_handle);
        bb_log_i(TAG, "bb_led_apa102: ok");
    } else {
        bb_log_w(TAG, "bb_led_apa102: open failed");
    }

    uint32_t boot_count = 0;
    bb_nv_get_u32("app", "boot", &boot_count, 0);
    boot_count++;
    bb_nv_set_u32("app", "boot", boot_count);
    bb_log_i(TAG, "boot=%lu", (unsigned long)boot_count);

    bb_wifi_set_hostname("bb-smoke");
    if (bb_wifi_init_sta() != BB_OK) {
        bb_log_e(TAG, "wifi assoc failed");
    }

    bb_http_server_start();
    bb_http_register_route(bb_http_server_get_handle(), BB_HTTP_GET, "/ping", ping_handler);
}

void smoke_app_loop(void) {
    bb_http_server_poll();
}
