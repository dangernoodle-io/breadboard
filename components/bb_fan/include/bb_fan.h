// bb_fan — panel-agnostic fan-controller + temperature HAL.
#pragma once
#include <stdint.h>
#include <stdbool.h>
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

// ---------------------------------------------------------------------------
// Autofan (opt-in: CONFIG_BB_FAN_AUTOFAN)
// BB fully owns fan duty in both modes when this feature is compiled in:
//   - enabled=true:  PID controller (Kp=5, Ki=0.1, Kd=2, REVERSE, P_ON_E,
//                    5000 ms sample time, dual EMA alpha=0.2) via set_duty_pct.
//   - enabled=false: manual_pct (clamped 0..100) applied each poll.
// Consumers never call bb_fan_set_duty_pct() for steady-state control.
// When compiled OUT (default), poll behavior is unchanged (no duty control).
// pid_input_src internal values: "die" or "aux"; GET /api/fan wire names: "die" or "vr".
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_FAN_AUTOFAN

// Autofan configuration. All fields in same units as TM's /api/fan.
typedef struct {
    bool  enabled;          // true = PID mode; false = manual_pct
    float die_target_c;     // ASIC die PID setpoint (°C)
    float aux_target_c;     // Aux/VR PID setpoint (°C); 0 = aux sensor ignored
    int   min_pct;          // Lower output limit [0..100]
    int   manual_pct;       // Duty when !enabled [0..100]
} bb_fan_autofan_cfg_t;

// Autofan telemetry snapshot (thread-safe; values are <0 / NAN until first tick).
typedef struct {
    float die_ema_c;        // EMA-filtered die temp; <0 = uninitialized
    float aux_ema_c;        // EMA-filtered aux temp; <0 = uninitialized or not fed
    float pid_input_c;      // Sensor temp fed to PID this tick; <0 = uninitialized
    const char *pid_input_src; // internal: "die" or "aux" (wire layer maps "aux"→"vr")
} bb_fan_autofan_telemetry_t;

// Apply autofan config. Thread-safe. Takes effect on next bb_fan_poll().
// Returns BB_ERR_INVALID_ARG if h is NULL.
bb_err_t bb_fan_set_autofan(bb_fan_handle_t h, const bb_fan_autofan_cfg_t *cfg);

// Copy the current autofan config into *out. Thread-safe.
// Returns BB_ERR_INVALID_ARG if h or out is NULL.
bb_err_t bb_fan_get_autofan_cfg(bb_fan_handle_t h, bb_fan_autofan_cfg_t *out);

// Inject a millisecond clock function into the PID. Called by the platform-specific
// init (e.g. bb_fan_autofan_inject_clock on ESP-IDF). Null = keep default (returns 0).
void bb_fan_autofan_set_clock(bb_fan_handle_t h, unsigned long (*fn)(void));

// ESP-IDF only: inject esp_timer-backed ms clock into the PID.
// Call once after bb_fan_handle_create (e.g. after bb_fan_emc2101_open).
// On host/Arduino this is a no-op stub (bb_fan_pid_set_mock_clock in tests).
#ifdef ESP_PLATFORM
void bb_fan_autofan_inject_clock(bb_fan_handle_t h);
#endif

// Feed aux (VR/secondary) temperature to the autofan controller.
// Call before or after bb_fan_poll(); stored under mutex.
// Pass a negative value to invalidate the reading (aux sensor absent/failed).
// This is the only bb_fan API that requires aux temp — bb_fan does NOT depend
// on bb_power; consumers (e.g. TM asic_task) feed it.
bb_err_t bb_fan_set_aux_temp(bb_fan_handle_t h, float aux_c);

// Copy autofan telemetry snapshot. Thread-safe.
void bb_fan_get_autofan_telemetry(bb_fan_handle_t h, bb_fan_autofan_telemetry_t *out);

#endif /* CONFIG_BB_FAN_AUTOFAN */

#ifdef __cplusplus
}
#endif
