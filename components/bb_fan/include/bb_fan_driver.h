// bb_fan_driver.h — vtable interface for bb_fan driver implementations.
// Consumers do NOT include this header; drivers depend on bb_fan and include it.
#pragma once
#include "bb_fan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Set fan duty cycle as a percentage (0..100). Clamped by the driver.
    bb_err_t (*set_duty_pct)(void *state, int pct);
    // Return the last duty pct set, or -1 if unknown.
    int      (*get_duty_pct)(void *state);
    // Read fan speed in RPM. Returns -1 on I2C error or stalled fan.
    int      (*read_rpm)(void *state);
    // Read external diode (die) temperature in degrees Celsius.
    // Returns BB_OK on success; any error → NAN sentinel in poll.
    bb_err_t (*read_die_temp_c)(void *state, float *out);
    // Read internal (board/ambient) temperature in degrees Celsius.
    // Returns BB_OK on success; any error → NAN sentinel in poll.
    bb_err_t (*read_board_temp_c)(void *state, float *out);
    // Static-lifetime identity string (e.g. "emc2101"). May be NULL.
    const char *name;
} bb_fan_driver_t;

// Called from a driver's _open after allocating its state struct.
// On success *out holds the opaque public handle. The bb_fan parent owns the
// wrapper allocation.
// drv must point to a static-lifetime vtable.
bb_err_t bb_fan_handle_create(const bb_fan_driver_t *drv, void *state,
                               bb_fan_handle_t *out);

#ifdef __cplusplus
}
#endif
