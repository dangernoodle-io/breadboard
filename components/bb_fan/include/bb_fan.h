// bb_fan — panel-agnostic fan-controller + temperature HAL.
#pragma once
#include <stdint.h>
#include <math.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bb_fan *bb_fan_handle_t;

// Snapshot of the last polled fan readings.
// rpm: -1 = unavailable or stalled.
// duty_pct: -1 = unavailable (not yet set or unknown).
// die_c: NAN = unavailable (external diode read failed or not supported).
// board_c: NAN = unavailable (internal sensor read failed or not supported).
typedef struct {
    int   rpm;
    int   duty_pct;
    float die_c;
    float board_c;
} bb_fan_snapshot_t;

// Read all channels via the vtable and cache the result.
// Thread-safe (mutex-protected write).
bb_err_t bb_fan_poll(bb_fan_handle_t h);

// Copy the cached snapshot into *out. Thread-safe (mutex-protected read).
// If h is NULL, rpm/duty_pct are set to -1 and die_c/board_c to NAN.
void bb_fan_snapshot(bb_fan_handle_t h, bb_fan_snapshot_t *out);

// Set fan duty cycle as a percentage (0..100). Delegates to drv->set_duty_pct.
bb_err_t bb_fan_set_duty_pct(bb_fan_handle_t h, int pct);

// Return the last known duty pct from the cached snapshot, or -1 if unknown.
int bb_fan_get_duty_pct(bb_fan_handle_t h);

// Return the driver's static name string, or NULL if h is invalid.
const char *bb_fan_name(bb_fan_handle_t h);

// Record h as the app's designated primary fan handle.
// Pass NULL to clear. Does not transfer ownership.
void bb_fan_set_primary(bb_fan_handle_t h);

// Return the handle recorded by bb_fan_set_primary(), or NULL if none set.
bb_fan_handle_t bb_fan_primary(void);

#ifdef __cplusplus
}
#endif
