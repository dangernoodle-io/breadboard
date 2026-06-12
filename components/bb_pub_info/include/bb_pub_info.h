// bb_pub_info — telemetry source satellite for device info / system metrics.
//
// Registers a bb_pub source under the "info" subtopic. On each tick, emits:
//   heap_internal_free    integer (bytes)
//   heap_internal_total   integer (bytes)
//   psram_free            integer (bytes; 0 on boards with no PSRAM)
//   psram_total           integer (bytes; 0 on boards with no PSRAM)
//   uptime_ms             integer (milliseconds since boot)
//   version               string  (bb_system_get_version())
//
// This source always publishes (never skips), providing a heartbeat even
// when no hardware HALs are present.
//
// Self-registration is gated on CONFIG_BB_PUB_INFO_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP tier
// at an order after bb_pub so the source registry exists first.
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "info" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 * Called automatically at PRE_HTTP tier when CONFIG_BB_PUB_INFO_AUTO_ATTACH=y.
 */
bb_err_t bb_pub_info_register(void);

#ifdef __cplusplus
}
#endif
