// bb_pub_fan — telemetry source satellite for bb_fan.
//
// Registers a bb_pub source under the "fan" subtopic. On each tick,
// samples bb_fan_primary() and emits:
//   rpm, duty_pct           integer or null when -1
//   die_c, board_c          number or null when NAN
// When CONFIG_BB_FAN_AUTOFAN is enabled, also emits:
//   die_ema_c, vr_ema_c, pid_input_c  number or null
//   pid_input_src                      string ("die" or "vr")
//
// Returns false (skip) when bb_fan_primary() is NULL.
//
// Self-registration is gated on CONFIG_BB_PUB_FAN_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP tier
// at an order after bb_pub so the source registry exists first.
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "fan" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 * Called automatically at PRE_HTTP tier when CONFIG_BB_PUB_FAN_AUTO_ATTACH=y.
 */
bb_err_t bb_pub_fan_register(void);

#ifdef __cplusplus
}
#endif
