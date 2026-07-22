#pragma once

#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_display's health.display cache/SSE surface (B1-893: re-homed from the
 * deleted bb_display_info satellite -- this surface is independent of
 * bb_info and stays live; the old /api/info "display" section died with
 * the satellite). Names kept as bb_display_info_* / bb_display_register_info
 * on relocation deliberately, for low churn -- not a residual bb_info dep.
 *
 * Call bb_display_register_info() after bb_display_init() and before
 * bb_http_server_start. The consumer must have registered a display
 * backend before calling this for a meaningful "present" snapshot.
 *
 * Presence of this header's registration call in the build is the opt-in
 * mechanism (handwire) or the bbtool:init marker below drives it via
 * codegen -- no Kconfig gate on the registration itself.
 */
void bb_display_register_info(void);

/*
 * Deferred registry-tier init: seeds the initial cache snapshot and bumps
 * the "health.display" bb_data generation (B1-1045). Attaching the key to
 * /api/events is a composition-root concern (bb_data_http_attach()). No-op
 * (returns BB_OK) if bb_display_register_info() was never called.
 */
// bbtool:init tier=regular fn=bb_display_info_register_init server=true
bb_err_t bb_display_info_register_init(bb_http_handle_t server);

#ifdef __cplusplus
}
#endif
