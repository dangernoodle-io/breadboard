// bb_thermal — pure value collector for aggregate thermal data.
//
// NOTE: GET /api/thermal route was deleted in B1-269 PR7. GET /api/sensors/
// thermal (bb_sensors, B1-828 PR-2) is the primary HTTP surface for thermal
// data -- its gather hook (bb_sensors_thermal_gather(),
// components/bb_sensors/bb_sensors_wire.c) calls bb_thermal_collect()
// directly. bb_thermal_init() is a no-op stub kept for link compatibility.
//
// Host twin: platform/host/bb_thermal/bb_thermal_host.c
#pragma once
#include <stdbool.h>
#include "bb_core.h"
#include "bb_http_server.h"

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
// SSOT: bb_sensors_thermal_gather() (/api/sensors/thermal) calls this.
void bb_thermal_collect(bb_thermal_values_t *out);

// No-op stub kept for link compatibility. /api/thermal route deleted in B1-269 PR7.
// bbtool:init tier=regular fn=bb_thermal_init server=true
bb_err_t bb_thermal_init(bb_http_handle_t server);

#ifdef BB_THERMAL_TESTING

#include "bb_temp_test.h"

// Reset state for test isolation.
void bb_thermal_reset_for_test(void);

#endif /* BB_THERMAL_TESTING */

#ifdef __cplusplus
}
#endif
