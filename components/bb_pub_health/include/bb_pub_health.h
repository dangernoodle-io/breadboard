// bb_pub_health — telemetry source satellite for /api/health fields.
//
// Registers a bb_pub source under the "health" subtopic. On each tick, emits:
//   ok            — bool  (true when validated and network is up)
//   mqtt_enabled  — bool  (bb_mqtt_default() != NULL)
//   mqtt_connected — bool (bb_mqtt_is_connected(bb_mqtt_default()))
//
// Fields already published by bb_pub_info (reset_reason, ota_validated) are
// not duplicated here. mqtt fields are omitted gracefully when bb_mqtt is absent.
//
// Always publishes (returns true) — provides a health heartbeat even without
// optional sinks.
//
// Self-registration is gated on CONFIG_BB_PUB_HEALTH_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP tier
// after bb_pub so the source registry exists first.
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "health" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 * Called automatically at PRE_HTTP tier when CONFIG_BB_PUB_HEALTH_AUTO_ATTACH=y.
 */
bb_err_t bb_pub_health_register(void);

#ifdef __cplusplus
}
#endif
