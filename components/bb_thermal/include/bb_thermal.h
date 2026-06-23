// bb_thermal — emit helper for aggregate thermal data (SSOT for /api/sensors thermal section).
//
// NOTE: GET /api/thermal route was deleted in B1-269 PR7.
// /api/sensors (bb_sensors) is the primary HTTP surface for thermal data.
// bb_thermal_init() is a no-op stub kept for link compatibility.
//
// Host twin: platform/host/bb_thermal/bb_thermal_host.c
#pragma once
#include <stdbool.h>
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Collected thermal values — one entry per source.
// present=false means the hardware is absent or reading failed; c is undefined when present=false.
// vr_hw_present / fan_hw_present distinguish "no hardware" from "hardware present but no reading".
typedef struct {
    bool  soc_present;      // true if SoC temp readable
    float soc_c;            // valid when soc_present

    bool  vr_hw_present;    // true if power primary != NULL
    bool  vr_present;       // true if vr_hw_present AND reading valid (temp_c >= 0)
    float vr_c;             // valid when vr_present

    bool  fan_hw_present;   // true if fan primary != NULL
    bool  asic_present;     // true if fan_hw_present AND die_c is not NaN
    float asic_c;           // valid when asic_present
    bool  board_present;    // true if fan_hw_present AND board_c is not NaN
    float board_c;          // valid when board_present
} bb_thermal_values_t;

// Pure value collector — reads snapshots from HAL, fills *out.
// Does NOT poll (callers must have already polled bb_power_poll / bb_fan_poll).
// SSOT: both bb_thermal_emit_section and bb_pub_thermal sample_fn call this.
void bb_thermal_collect(bb_thermal_values_t *out);

// No-op stub kept for link compatibility. /api/thermal route deleted in B1-269 PR7.
bb_err_t bb_thermal_init(bb_http_handle_t server);

// Shared emit helper — writes thermal sub-objects into an existing bb_json_t.
// Emits {soc,vr,asic,board} each as {present,c|null} (nested shape).
// Called by /api/sensors thermal section get_fn (SSOT).
void bb_thermal_emit_section(bb_json_t obj);

#ifdef BB_THERMAL_TESTING

#include "bb_temp_test.h"

// Reset state for test isolation.
void bb_thermal_reset_for_test(void);

#endif /* BB_THERMAL_TESTING */

#ifdef __cplusplus
}
#endif
