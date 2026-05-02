#include "bb_display.h"
#include "bb_display_backend.h"
#include "bb_log.h"

#include <string.h>

static const char *TAG = "bb_display";

static const bb_display_backend_t *s_backend = NULL;
static bool     s_ready = false;
static uint16_t s_width = 0;
static uint16_t s_height = 0;

void bb_display_register_backend(const bb_display_backend_t *backend)
{
    if (!backend) return;
    s_backend = backend;
}

bb_err_t bb_display_init(void)
{
    if (s_ready) return BB_OK;
    if (!s_backend || !s_backend->init) {
        bb_log_w(TAG, "no backend registered — display API is a no-op");
        return BB_ERR_INVALID_STATE;
    }

    uint16_t w = 0, h = 0;
    bb_err_t err = s_backend->init(&w, &h);
    if (err != BB_OK) {
        bb_log_e(TAG, "backend %s init failed: %d",
                 s_backend->name ? s_backend->name : "?", (int)err);
        return err;
    }
    s_width  = w;
    s_height = h;
    s_ready  = true;
    bb_log_i(TAG, "init: %ux%u (%s)", (unsigned)w, (unsigned)h,
             s_backend->name ? s_backend->name : "?");
    return BB_OK;
}

bool     bb_display_ready(void)  { return s_ready; }
uint16_t bb_display_width(void)  { return s_width; }
uint16_t bb_display_height(void) { return s_height; }

void bb_display_clear(uint16_t rgb565)
{
    if (!s_ready || !s_backend->clear) return;
    s_backend->clear(rgb565);
}

void bb_display_blit(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    if (!s_ready || !s_backend->blit || !pixels || !w || !h) return;
    s_backend->blit(x, y, w, h, pixels);
}

void bb_display_flush(void)
{
    if (!s_ready || !s_backend->flush) return;
    s_backend->flush();
}

void bb_display_off(void)
{
    if (!s_ready) return;
    if (s_backend->off) s_backend->off();
    s_ready = false;
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
    if (!font) font = &bb_display_font_8x16;

    if (s_backend->draw_text) {
        s_backend->draw_text(x, y, text, font, fg, bg);
        return;
    }
    if (!s_backend->blit) return;
    if (font->glyph_w > BB_DISPLAY_RASTER_MAX_W ||
        font->glyph_h > BB_DISPLAY_RASTER_MAX_H) return;

    uint16_t glyph[BB_DISPLAY_RASTER_MAX_W * BB_DISPLAY_RASTER_MAX_H];
    int16_t cx = x;
    for (const char *p = text; *p; p++) {
        rasterize_glyph(font, (uint8_t)*p, fg, bg, glyph);
        s_backend->blit(cx, y, font->glyph_w, font->glyph_h, glyph);
        cx += font->glyph_w;
    }
}

static void render_centered_lines(const char * const *lines, size_t n,
                                  const bb_display_font_t *font,
                                  uint16_t fg, uint16_t bg)
{
    if (!s_ready || n == 0) return;
    if (!font) font = &bb_display_font_8x16;
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
