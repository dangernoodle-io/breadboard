// bb_pub_power — telemetry source satellite for bb_power.
//
// Registers a bb_pub source under the "power" subtopic. On each tick,
// samples bb_power_primary() and emits:
//   vout_mv, iout_ma, pout_mw, vin_mv, temp_c   integer or null when -1
//
// Returns false (skip) when bb_power_primary() is NULL.
//
// Self-registration is gated on CONFIG_BB_PUB_POWER_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP tier
// at an order after bb_pub so the source registry exists first.
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "power" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 * Called automatically at PRE_HTTP tier when CONFIG_BB_PUB_POWER_AUTO_ATTACH=y.
 */
bb_err_t bb_pub_power_register(void);

#ifdef __cplusplus
}
#endif
