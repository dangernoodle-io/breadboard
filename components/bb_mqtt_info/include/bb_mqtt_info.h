#pragma once

#include "bb_core.h"
#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_mqtt_info — satellite component that surfaces MQTT connection status
 * in /api/health as a named section.
 *
 * Include this component (REQUIRES bb_mqtt_info) when you want:
 *  - bb_mqtt_register_health(): to expose a "mqtt" section on /api/health with
 *    a schema contributed via bb_health_register_section.
 *
 * Call bb_mqtt_register_health() before bb_http_server_start (before the
 * health section table is frozen).
 *
 * Presence of this satellite component in the build (via REQUIRES) is the
 * opt-in mechanism — no Kconfig gate is needed.
 *
 * Reported fields (as nested "mqtt" section):
 *   "mqtt": {
 *     "enabled":   bool  — true when bb_mqtt_client_default() is non-NULL
 *                          (MQTT was configured and started)
 *     "connected": bool  — true when the client is currently connected to
 *                          the broker (bb_mqtt_client_is_connected)
 *   }
 *
 * No secrets are exposed.
 */

/*
 * Register a /api/health section named "mqtt" that emits:
 *   { "enabled": <bool>, "connected": <bool> }
 * Also contributes a JSON-Schema value to the /api/health 200 response schema
 * via bb_health_register_section.
 * Call before bb_http_server_start.
 */
void bb_mqtt_register_health(void);

/**
 * Registry hook — calls bb_mqtt_register_health(). server is unused
 * (bb_mqtt_info has no HTTP routes of its own, only a /api/health section).
 */
// bbtool:init tier=regular fn=bb_mqtt_info_autoregister_init server=true
bb_err_t bb_mqtt_info_autoregister_init(bb_http_handle_t server);

#ifdef __cplusplus
}
#endif
