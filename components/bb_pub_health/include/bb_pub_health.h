// bb_pub_health — telemetry source satellite for /api/health fields.
//
// Registers a bb_pub source under the "health" subtopic. On each tick, emits:
//   ok            — bool  (true when validated and network is up)
//   mqtt_enabled  — bool  (bb_mqtt_client_default() != NULL)
//   mqtt_connected — bool (bb_mqtt_client_is_connected(bb_mqtt_client_default()))
//
// Fields already published by bb_pub_info (reset_reason, ota_validated) are
// not duplicated here. mqtt fields are omitted gracefully when bb_mqtt_client is absent.
//
// Always publishes (returns true) — provides a health heartbeat even without
// optional sinks.
//
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "health" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 */
bb_err_t bb_pub_health_register(void);

/**
 * PRE_HTTP init entry point (after bb_pub): registers the "health" source.
 */
// bbtool:init tier=pre_http fn=bb_pub_health_init
bb_err_t bb_pub_health_init(void);

#ifdef __cplusplus
}
#endif
