#pragma once

#include "bb_http_server.h"

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

/*
 * Deferred registry-tier init: attaches the "health.display" topic to
 * /api/events and seeds the initial cache snapshot. No-op (returns BB_OK)
 * unless CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH is set.
 */
// bbtool:init tier=regular fn=bb_display_info_register_init server=true
bb_err_t bb_display_info_register_init(bb_http_handle_t server);

#ifdef __cplusplus
}
#endif
