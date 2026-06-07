#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_temp — satellite component that reads the SoC internal temperature sensor
 * and optionally registers a /api/health extender.
 *
 * Include this component (REQUIRES bb_temp) when you want:
 *  - bb_temp_read_soc(): a portable wrapper around the SoC die-temperature
 *    sensor (ESP32-S2/S3/C3/C6/H2/... only; returns false on unsupported parts
 *    such as the classic ESP32 WROOM-32).
 *  - bb_temp_register_info(): to expose a "temp" object on /api/health with
 *    a schema fragment contributed via bb_health_register_extender_ex.
 *
 * Call bb_temp_register_info() before bb_http_server_start (before the health
 * extender table is frozen).
 *
 * Presence of this satellite component in the build (via REQUIRES) is the
 * opt-in mechanism — no Kconfig gate is needed.
 */

#include <stdbool.h>

/*
 * Read the SoC internal die temperature.
 * Returns true and writes *out_celsius when the sensor is supported and
 * the read succeeds; returns false otherwise (caller treats false as absent).
 * *out_celsius is untouched on false.
 */
bool bb_temp_read_soc(float *out_celsius);

/*
 * Register a /api/health extender that emits a "temp" object:
 *   { "present": <bool> [, "soc_c": <number>] }
 * Also contributes a JSON-Schema properties fragment to the /api/health
 * 200 response schema via bb_health_register_extender_ex.
 * Call before bb_http_server_start.
 */
void bb_temp_register_info(void);

#ifdef __cplusplus
}
#endif
