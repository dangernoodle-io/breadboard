#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_display_info — satellite component that registers a /api/info extender
 * reporting display state.
 *
 * Include this component (REQUIRES bb_display_info) when you want the
 * /api/info response to include a nested "display" object:
 *   { present, panel, width, height, enabled }
 *
 * Call bb_display_register_info() before bb_http_server_start (before the
 * extender table is frozen). The consumer must also init bb_display and
 * register a backend before calling this.
 *
 * Presence of this satellite component in the build (via REQUIRES) is the
 * opt-in mechanism — no Kconfig gate is needed.
 */
void bb_display_register_info(void);

#ifdef __cplusplus
}
#endif
