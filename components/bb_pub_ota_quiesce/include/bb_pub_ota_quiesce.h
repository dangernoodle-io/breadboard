// bb_pub_ota_quiesce — glue companion that wires bb_pub pause/resume into the
// update-check and OTA-pull HTTP pause hooks so that telemetry publishing is
// quiesced during the TLS handshake, freeing heap for heap-tight targets like
// the ESP32-S2.
//
// Calling bb_pub_ota_quiesce_init() installs:
//   bb_update_check_set_hooks(bb_pub_quiesce_pause, bb_pub_quiesce_resume)
//   bb_ota_set_hooks(bb_pub_quiesce_pause, bb_pub_quiesce_resume)
//
// Self-registration is gated on CONFIG_BB_PUB_QUIESCE_ON_OTA (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP tier.
//
// bb_pub layering is preserved: this component depends on bb_pub, bb_update_check,
// and bb_ota_hooks — none of those depend back on this component.
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wire bb_pub_pause / bb_pub_resume into update-check and OTA pause hooks.
 * Idempotent. Call after bb_update_check_init() has run (returns
 * BB_ERR_INVALID_STATE if update-check is not yet initialised).
 * Called automatically at the PRE_HTTP tier when CONFIG_BB_PUB_QUIESCE_ON_OTA=y.
 */
bb_err_t bb_pub_ota_quiesce_init(void);

#ifdef __cplusplus
}
#endif
