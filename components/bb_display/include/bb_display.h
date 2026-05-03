#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bb_core.h"

/*
 * bb_display — panel-agnostic display API.
 *
 * Core surface is pixel-buffer primitives only (clear / blit / flush)
 * so consumers are never locked into a particular text/font/size
 * model. Optional layered helpers (draw_text, show_splash, show_prov)
 * are built on top of blit and accept a swappable font, defaulting to
 * the bundled 8x16 ASCII bitmap when font is NULL.
 *
 * Wiring: panel drivers ship as `bb_display_<chip>` components (e.g.
 * bb_display_ek79007 for the LVGL+MIPI-DSI EK79007) and register a
 * `bb_display_backend_t` (see bb_display_backend.h) at link time. A
 * consumer adds the backend(s) it needs to REQUIRES; a headless
 * consumer adds neither bb_display nor any backend, paying nothing.
 *
 * Multi-backend in one image is allowed (last register wins at init);
 * most builds link exactly one.
 */

bb_err_t bb_display_init(void);
void     bb_display_off(void);
bool     bb_display_ready(void);

uint16_t bb_display_width(void);
uint16_t bb_display_height(void);

/* Fill the entire framebuffer with `rgb565`. Synchronous-ness is
 * backend-defined; call bb_display_flush() if you need the panel
 * updated before returning. */
void bb_display_clear(uint16_t rgb565);

/* Write a rectangle of RGB565 pixels into the framebuffer. `pixels`
 * is row-major, length w*h, little-endian RGB565 per pixel. Out-of-
 * bounds rectangles are clipped, not an error. Backends that map
 * RGB565 to a different native format (e.g. monochrome OLED) convert
 * internally. */
void bb_display_blit(int16_t x, int16_t y,
                     uint16_t w, uint16_t h,
                     const uint16_t *pixels);

/* Push any pending writes to the panel. Backends that update the
 * panel on every primitive treat this as a no-op. */
void bb_display_flush(void);

/* -------- Optional layered helpers (built on blit) -------- */

/*
 * Bitmap font descriptor. `bitmap` packs `glyph_count` glyphs back to
 * back; each glyph is `glyph_h` bytes, one byte per row, MSB =
 * leftmost pixel. Glyphs cover codepoints
 * [first_codepoint, first_codepoint + glyph_count). Out-of-range
 * codepoints render as blanks.
 *
 * Width per glyph must fit in 8 bits (one byte per row); for wider
 * glyphs add a multi-byte-row variant later.
 */
typedef struct {
    uint8_t  glyph_w;
    uint8_t  glyph_h;
    uint8_t  first_codepoint;
    uint8_t  glyph_count;
    const uint8_t *bitmap;
} bb_display_font_t;

/* Bundled fonts. Availability depends on Kconfig/compile flags. */
#ifndef BB_DISPLAY_FONT_8X16
#define BB_DISPLAY_FONT_8X16 1
#endif
#ifndef BB_DISPLAY_FONT_6X8
#define BB_DISPLAY_FONT_6X8 1
#endif
#ifndef BB_DISPLAY_FONT_5X7
#define BB_DISPLAY_FONT_5X7 1
#endif

#if BB_DISPLAY_FONT_8X16
extern const bb_display_font_t bb_display_font_8x16;
#endif
#if BB_DISPLAY_FONT_6X8
extern const bb_display_font_t bb_display_font_6x8;
#endif
#if BB_DISPLAY_FONT_5X7
extern const bb_display_font_t bb_display_font_5x7;
#endif

/* Render `text` at (x,y) using `font` (NULL → bb_display_font_8x16).
 * Backends with a native text engine (e.g. LVGL) may implement this
 * directly; others fall back to a per-glyph rasterizer + blit. */
void bb_display_draw_text(int16_t x, int16_t y, const char *text,
                          const bb_display_font_t *font,
                          uint16_t fg_rgb565, uint16_t bg_rgb565);

/* Clear and render a centered two-line splash. */
void bb_display_show_splash(const char *product, const char *version,
                            const bb_display_font_t *font);

/* Clear and render a centered three-line provisioning screen
 * ("Provisioning" / ssid / pass). */
void bb_display_show_prov(const char *ap_ssid, const char *ap_pass,
                          const bb_display_font_t *font);

/* Set the fallback font used when callers pass NULL to draw_text,
 * show_splash, show_prov. Pass NULL to restore the compile-time default. */
void bb_display_set_default_font(const bb_display_font_t *font);

/* Rotate the display. Valid values: 0, 90, 180, 270.
 * Returns BB_ERR_INVALID_STATE if the active backend doesn't support this
 * angle, or BB_ERR_INVALID_ARG for non-cardinal values. */
bb_err_t bb_display_set_rotation(uint16_t deg);
