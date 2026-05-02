#pragma once
#include <stdint.h>
#include "bb_core.h"
#include "bb_display.h"

/*
 * bb_display backend interface.
 *
 * Each panel driver ships as its own component (`bb_display_<chip>`)
 * and registers a `bb_display_backend_t` via a file-scope constructor.
 * `init` reports the panel's native pixel dimensions through `*w`/`*h`
 * so the core can stash them for `bb_display_width/_height`.
 *
 * Required: init, clear, blit, off.
 * Optional: flush (NULL = no-op), draw_text (NULL = core rasterizes
 * via the active font and blit).
 *
 * The constructor must be kept link-live by the
 * `bb_registry_force_register` CMake helper on PlatformIO builds
 * (which strip --whole-archive); see cmake/bb_registry.cmake for the
 * pattern (use a backend-specific symbol name, e.g.
 * bb_display_register__ek79007).
 */

typedef struct bb_display_backend_s {
    const char *name;
    bb_err_t (*init)(uint16_t *width_out, uint16_t *height_out);
    void     (*clear)(uint16_t rgb565);
    void     (*blit)(int16_t x, int16_t y,
                     uint16_t w, uint16_t h,
                     const uint16_t *pixels);
    void     (*flush)(void);
    void     (*off)(void);
    void     (*draw_text)(int16_t x, int16_t y, const char *text,
                          const bb_display_font_t *font,
                          uint16_t fg_rgb565,
                          uint16_t bg_rgb565);
} bb_display_backend_t;

/* Register a backend. Last-wins. Safe from a constructor before main. */
void bb_display_register_backend(const bb_display_backend_t *backend);
