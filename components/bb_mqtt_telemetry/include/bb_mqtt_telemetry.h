// bb_mqtt_telemetry — MQTT section provider for /api/telemetry.
//
// Registers a "mqtt" section via bb_telemetry_register_section.
// GET reads NVS "bb_mqtt" and reports connection state (masked secrets).
// PATCH persists fields to NVS "bb_mqtt" (including TLS PEM material).
//
// Host twin: platform/host/bb_mqtt_telemetry/bb_mqtt_telemetry_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"
#include "bb_mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the "mqtt" telemetry section.
// Called automatically when CONFIG_BB_MQTT_TELEMETRY_AUTOREGISTER=y (PRE_HTTP tier).
bb_err_t bb_mqtt_telemetry_init(void);

// Set the client handle reference used to report connection state.
// Pass a pointer to the module-level bb_mqtt_client_t handle (or NULL to clear).
void bb_mqtt_telemetry_set_client(bb_mqtt_client_t *ref);

#ifdef BB_MQTT_TELEMETRY_TESTING

// Reset state for test isolation.
void bb_mqtt_telemetry_reset_for_test(void);

// Expose section get/patch for direct test invocation.
void     bb_mqtt_telemetry_section_get_for_test(bb_json_t section, void *ctx);
bb_err_t bb_mqtt_telemetry_section_patch_for_test(bb_json_t patch, void *ctx);

#endif /* BB_MQTT_TELEMETRY_TESTING */

#ifdef __cplusplus
}
#endif
