// bb_power — panel-agnostic voltage-regulator monitor HAL.
#pragma once
#include <stdint.h>
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bb_power *bb_power_handle_t;

// Snapshot of the last polled VR readings. -1 means unavailable.
typedef struct {
    int vout_mv;
    int iout_ma;
    int pout_mw;  // (vout_mv * iout_ma) / 1000 if both ≥0, else -1
    int vin_mv;
    int temp_c;
} bb_power_snapshot_t;

// Read all channels via the vtable and cache the result.
// Thread-safe (mutex-protected write).
bb_err_t bb_power_poll(bb_power_handle_t h);

// Copy the cached snapshot into *out. Thread-safe (mutex-protected read).
// If h is NULL, all fields are set to -1 and present=false semantics apply.
void bb_power_snapshot(bb_power_handle_t h, bb_power_snapshot_t *out);

// Change the output voltage. Delegates to drv->set_vout_mv.
bb_err_t bb_power_set_vout_mv(bb_power_handle_t h, uint16_t mv);

// Return the driver's static name string, or NULL if h is invalid.
const char *bb_power_name(bb_power_handle_t h);

// Record h as the app's designated primary power handle.
// Pass NULL to clear. Does not transfer ownership.
void bb_power_set_primary(bb_power_handle_t h);

// Return the handle recorded by bb_power_set_primary(), or NULL if none set.
bb_power_handle_t bb_power_primary(void);

// ---------------------------------------------------------------------------
// JSON serializer — single builder used by REST responses.
// ---------------------------------------------------------------------------

/**
 * Emit power fields from snap into the JSON object obj.
 *
 * Emits: vout_mv, iout_ma, pout_mw, vin_mv, temp_c.
 * Each field is a number when the value is >= 0, null when -1.
 *
 * Pure, host-testable — no ESP-IDF dependencies.
 */
void bb_power_emit(bb_json_t obj, const bb_power_snapshot_t *snap);

// Shared emit helper — writes "present" plus power fields into an existing
// bb_json_t object. SSOT for the /api/sensors power section (bb_sensors).
void bb_power_emit_section(bb_json_t obj);

#ifdef __cplusplus
}
#endif
