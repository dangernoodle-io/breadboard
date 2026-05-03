#pragma once
#include <stdint.h>
#include "bb_core.h"
#include "bb_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_display backend interface.
 *
 * Each panel driver ships as its own component (`bb_display_<chip>`)
 * and registers a `bb_display_backend_t` via a file-scope constructor.
 * Backends are stored in an ordered list (insertion order). During
 * `bb_display_init`, the core walks this list in order: for each backend,
 * if `probe` is non-NULL, it's called first (cheap HW detection); if it
 * returns BB_OK, init is called; if init succeeds, that backend is
 * selected and initialization completes. If no backend succeeds, BB_ERR_NOT_FOUND
 * is returned.
 *
 * `probe`: Optional. If NULL, treated as "always succeeds" (preserves
 * single-backend builds). Return BB_OK if HW is present, BB_ERR_NOT_FOUND
 * if probably-absent, or any other error if the probe attempt itself failed.
 *
 * `init`: Required. Reports the panel's native pixel dimensions through
 * `*w`/`*h` so the core can stash them for `bb_display_width/_height`.
 *
 * Required: init, clear, blit, off.
 * Optional: probe (NULL = always-present), flush (NULL = no-op),
 * draw_text (NULL = core rasterizes via the active font and blit).
 *
 * The constructor must be kept link-live by the
 * `bb_registry_force_register` CMake helper on PlatformIO builds
 * (which strip --whole-archive); see cmake/bb_registry.cmake for the
 * pattern (use a backend-specific symbol name, e.g.
 * bb_display_register__ek79007).
 */

typedef struct bb_display_backend_s {
    const char *name;
    bb_err_t (*probe)(void);  /* Optional: cheap HW detection. NULL = always succeeds. */
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
    bb_err_t (*set_rotation)(uint16_t deg, uint16_t *new_w, uint16_t *new_h);  /* Optional. */
} bb_display_backend_t;

/* Register a backend. Appends to ordered list. Safe from a constructor before main. */
void bb_display_register_backend(const bb_display_backend_t *backend);

#ifdef __cplusplus
}
#endif
