#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_temp — satellite component that reads the SoC internal temperature sensor
 * and optionally registers a /api/health section.
 *
 * Include this component (REQUIRES bb_temp) when you want:
 *  - bb_temp_read_soc(): a portable wrapper around the SoC die-temperature
 *    sensor (ESP32-S2/S3/C3/C6/H2/... only; returns false on unsupported parts
 *    such as the classic ESP32 WROOM-32).
 *  - bb_temp_register_info(): to expose a "temp" section on /api/health with
 *    a schema contributed via bb_health_register_section.
 *
 * Call bb_temp_register_info() before bb_http_server_start (before the health
 * section table is frozen).
 *
 * Presence of this satellite component in the build (via REQUIRES) is the
 * opt-in mechanism — no Kconfig gate is needed.
 */

#include <stdbool.h>
#include "bb_json.h"

/*
 * Read the SoC internal die temperature.
 * Returns true and writes *out_celsius when the sensor is supported and
 * the read succeeds; returns false otherwise (caller treats false as absent).
 * *out_celsius is untouched on false.
 */
bool bb_temp_read_soc(float *out_celsius);

/*
 * Emit the temp JSON section into obj.
 * Calls bb_temp_read_soc() and writes:
 *   { "present": true,  "soc_c": <rounded-1dp> }  when sensor available
 *   { "present": false }                            when absent
 * SSOT formatter — called by bb_temp_register_info's get_fn and by any
 * future bb_pub source. Reads live; does NOT poll.
 */
void bb_temp_emit_section(bb_json_t obj);

/*
 * Register a /api/health section named "temp" that emits:
 *   { "present": <bool> [, "soc_c": <number>] }
 * Also contributes a JSON-Schema value to the /api/health 200 response schema
 * via bb_health_register_section.
 * Call before bb_http_server_start.
 */
void bb_temp_register_info(void);

#ifdef __cplusplus
}
#endif
