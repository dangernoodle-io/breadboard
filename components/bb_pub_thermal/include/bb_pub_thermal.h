// bb_pub_thermal — telemetry source satellite for aggregate temperatures.
//
// Registers a bb_pub source under the "thermal" subtopic. On each tick,
// aggregates temperature readings from multiple HALs:
//   soc_c     number or null — bb_temp_read_soc()
//   vr_c      number or null — bb_power_primary() .temp_c
//   asic_c    number or null — bb_fan_primary()   .die_c
//   board_c   number or null — bb_fan_primary()   .board_c
//
// Returns false (skip) when all four sources are unavailable.
//
// Self-registration is gated on CONFIG_BB_PUB_THERMAL_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP tier
// at an order after bb_pub so the source registry exists first.
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "thermal" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 * Called automatically at PRE_HTTP tier when CONFIG_BB_PUB_THERMAL_AUTO_ATTACH=y.
 */
bb_err_t bb_pub_thermal_register(void);

#ifdef __cplusplus
}
#endif
