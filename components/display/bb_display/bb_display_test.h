#pragma once

#ifdef BB_DISPLAY_TESTING

/*
 * Test-only reset hook. Clears all registered backends and active state so
 * each unit test starts from a clean slate. Not part of the public API.
 */
void bb_display_reset_for_testing(void);

/*
 * Nulls s_default_font without touching other state. Used to exercise the
 * !font guard in draw_text / render_centered_lines when the caller passes
 * NULL and no compile-time or runtime default is available.
 */
void bb_display_clear_default_font_for_testing(void);

#endif /* BB_DISPLAY_TESTING */
