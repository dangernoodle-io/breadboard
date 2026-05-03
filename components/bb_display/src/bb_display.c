#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_log.h"

#include <string.h>

static const char *TAG = "bb_display";

#define BB_DISPLAY_MAX_BACKENDS 8
static const bb_display_backend_t *s_backends[BB_DISPLAY_MAX_BACKENDS];
static size_t s_backend_count = 0;
static const bb_display_backend_t *s_active = NULL;
static bool     s_ready = false;
static uint16_t s_width = 0;
static uint16_t s_height = 0;

/* Default font selection: prefer largest available. */
#if BB_DISPLAY_FONT_8X16
static const bb_display_font_t *s_compile_time_default_font = &bb_display_font_8x16;
#elif BB_DISPLAY_FONT_6X8
static const bb_display_font_t *s_compile_time_default_font = &bb_display_font_6x8;
#elif BB_DISPLAY_FONT_5X7
static const bb_display_font_t *s_compile_time_default_font = &bb_display_font_5x7;
#else
static const bb_display_font_t *s_compile_time_default_font = NULL;
#endif

static const bb_display_font_t *s_default_font = NULL;  /* Initialized in bb_display_init(). */

void bb_display_register_backend(const bb_display_backend_t *backend)
{
    if (!backend) return;
    if (s_backend_count >= BB_DISPLAY_MAX_BACKENDS) {
        bb_log_w(TAG, "backend registry full; dropping %s", backend->name ? backend->name : "?");
        return;
    }
    s_backends[s_backend_count++] = backend;
}

bb_err_t bb_display_init(void)
{
    if (s_ready) return BB_OK;
    if (s_backend_count == 0) {
        bb_log_w(TAG, "no backend registered — display API is a no-op");
        return BB_ERR_INVALID_STATE;
    }

    /* Initialize default font on first init. */
    if (s_default_font == NULL) {
        s_default_font = s_compile_time_default_font;
    }

    for (size_t i = 0; i < s_backend_count; i++) {
        const bb_display_backend_t *candidate = s_backends[i];
        if (!candidate || !candidate->init) continue;

        /* Call probe if available. */
        bool probed = false;
        if (candidate->probe) {
            bb_err_t probe_err = candidate->probe();
            probed = true;
            if (probe_err != BB_OK) {
                bb_log_i(TAG, "probe %s: not found", candidate->name ? candidate->name : "?");
                continue;
            }
        }

        /* Try init. */
        uint16_t w = 0, h = 0;
        bb_err_t err = candidate->init(&w, &h);
        if (err != BB_OK) {
            bb_log_w(TAG, "init %s failed: %d", candidate->name ? candidate->name : "?", (int)err);
            continue;
        }

        /* Success. */
        s_active = candidate;
        s_width = w;
        s_height = h;
        s_ready = true;
        bb_log_i(TAG, "init: %ux%u (%s) [probed=%s]", (unsigned)w, (unsigned)h,
                 candidate->name ? candidate->name : "?", probed ? "yes" : "no");
        return BB_OK;
    }

    bb_log_e(TAG, "no backend succeeded initialization");
    return BB_ERR_INVALID_STATE;
}

bool     bb_display_ready(void)  { return s_ready; }
uint16_t bb_display_width(void)  { return s_width; }
uint16_t bb_display_height(void) { return s_height; }

void bb_display_clear(uint16_t rgb565)
{
    if (!s_ready || !s_active->clear) return;
    s_active->clear(rgb565);
}

void bb_display_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    if (!s_ready || !s_active->blit || !pixels || !w || !h) return;
    s_active->blit(x, y, w, h, pixels);
}

void bb_display_flush(void)
{
    if (!s_ready || !s_active->flush) return;
    s_active->flush();
}

void bb_display_off(void)
{
    if (!s_ready) return;
    if (s_active->off) s_active->off();
    s_ready = false;
    s_active = NULL;
    s_width = 0;
    s_height = 0;
}

/* -------- Layered helpers — font rasterizer fallback -------- */

/* Glyph rasterization buffer. Sized for the bundled 8x16 font; bigger
 * glyphs allocate via VLA / static-cap. Keep small to avoid stack
 * pressure on backends that pass through to LVGL. */
#define BB_DISPLAY_RASTER_MAX_W 32
#define BB_DISPLAY_RASTER_MAX_H 32

static void rasterize_glyph(const bb_display_font_t *font, uint8_t cp,
                            uint16_t fg, uint16_t bg, uint16_t *out)
{
    /* Out is row-major glyph_w * glyph_h pixels. */
    const uint8_t *rows = NULL;
    if (cp >= font->first_codepoint &&
        (cp - font->first_codepoint) < font->glyph_count) {
        rows = &font->bitmap[(cp - font->first_codepoint) * font->glyph_h];
    }
    for (uint8_t row = 0; row < font->glyph_h; row++) {
        uint8_t bits = rows ? rows[row] : 0;
        for (uint8_t col = 0; col < font->glyph_w; col++) {
            bool on = (bits >> (7 - col)) & 0x1;
            out[row * font->glyph_w + col] = on ? fg : bg;
        }
    }
}

void bb_display_draw_text(int16_t x, int16_t y, const char *text,
                          const bb_display_font_t *font,
                          uint16_t fg, uint16_t bg)
{
    if (!s_ready || !text) return;
    if (!font) font = s_default_font;
    if (!font) return;

    if (s_active->draw_text) {
        s_active->draw_text(x, y, text, font, fg, bg);
        return;
    }
    if (!s_active->blit) return;
    if (font->glyph_w > BB_DISPLAY_RASTER_MAX_W ||
        font->glyph_h > BB_DISPLAY_RASTER_MAX_H) return;

    uint16_t glyph[BB_DISPLAY_RASTER_MAX_W * BB_DISPLAY_RASTER_MAX_H];
    int16_t cx = x;
    for (const char *p = text; *p; p++) {
        rasterize_glyph(font, (uint8_t)*p, fg, bg, glyph);
        s_active->blit(cx, y, font->glyph_w, font->glyph_h, glyph);
        cx += font->glyph_w;
    }
}

static void render_centered_lines(const char * const *lines, size_t n,
                                  const bb_display_font_t *font,
                                  uint16_t fg, uint16_t bg)
{
    if (!s_ready || n == 0) return;
    if (!font) font = s_default_font;
    if (!font) return;
    int total_h = (int)n * font->glyph_h;
    int y0 = ((int)s_height - total_h) / 2;
    if (y0 < 0) y0 = 0;
    for (size_t i = 0; i < n; i++) {
        const char *t = lines[i] ? lines[i] : "";
        int text_w = (int)strlen(t) * font->glyph_w;
        int x = ((int)s_width - text_w) / 2;
        if (x < 0) x = 0;
        bb_display_draw_text((int16_t)x,
                             (int16_t)(y0 + (int)i * font->glyph_h),
                             t, font, fg, bg);
    }
    bb_display_flush();
}

void bb_display_show_splash(const char *product, const char *version,
                            const bb_display_font_t *font)
{
    if (!s_ready) return;
    bb_display_clear(0x0004);
    const char *lines[2] = { product ? product : "", version ? version : "" };
    render_centered_lines(lines, 2, font, 0xFFFF, 0x0004);
}

void bb_display_show_prov(const char *ap_ssid, const char *ap_pass,
                          const bb_display_font_t *font)
{
    if (!s_ready) return;
    bb_display_clear(0x0140);
    const char *lines[3] = {
        "Provisioning",
        ap_ssid ? ap_ssid : "",
        ap_pass ? ap_pass : "",
    };
    render_centered_lines(lines, 3, font, 0xFFFF, 0x0140);
}

void bb_display_set_default_font(const bb_display_font_t *font)
{
    if (font == NULL) {
        s_default_font = s_compile_time_default_font;
    } else {
        s_default_font = font;
    }
}

bb_err_t bb_display_set_rotation(uint16_t deg)
{
    if (!s_ready || !s_active) return BB_ERR_INVALID_STATE;
    if (deg != 0 && deg != 90 && deg != 180 && deg != 270) return BB_ERR_INVALID_ARG;
    if (!s_active->set_rotation) return BB_ERR_INVALID_STATE;

    uint16_t nw = s_width, nh = s_height;
    bb_err_t err = s_active->set_rotation(deg, &nw, &nh);
    if (err == BB_OK) {
        s_width = nw;
        s_height = nh;
    }
    return err;
}
