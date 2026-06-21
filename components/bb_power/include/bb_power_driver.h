// bb_power_driver.h — vtable interface for bb_power driver implementations.
// Consumers do NOT include this header; drivers depend on bb_power and include it.
#pragma once
#include "bb_power.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Read output voltage in millivolts. Returns mV, or -1 on error.
    int (*read_vout_mv)(void *state);
    // Read output current in milliamps. Returns mA, or -1 on error.
    int (*read_iout_ma)(void *state);
    // Read input voltage in millivolts. Returns mV, or -1 on error.
    int (*read_vin_mv)(void *state);
    // Read die temperature in degrees Celsius. Returns degC, or -1 on error.
    int (*read_temp_c)(void *state);
    // Set output voltage in millivolts.
    bb_err_t (*set_vout_mv)(void *state, uint16_t mv);
    // Optional per-poll hook called by bb_power_poll after all channel reads.
    // Best-effort: errors are ignored. May be NULL.
    void (*poll)(void *state);
    // Static-lifetime identity string (e.g. "tps546"). May be NULL.
    const char *name;
} bb_power_driver_t;

// Called from a driver's _open after allocating its state struct.
// On success *out holds the opaque public handle. The bb_power parent owns the
// wrapper allocation.
// drv must point to a static-lifetime vtable.
bb_err_t bb_power_handle_create(const bb_power_driver_t *drv, void *state,
                                 bb_power_handle_t *out);

// Returns the driver-private state pointer stored in the handle.
// Used by drivers that need to call back into their own state (e.g. recover()).
// Returns NULL if h is NULL.
void *bb_power_handle_state(bb_power_handle_t h);

#ifdef __cplusplus
}
#endif
