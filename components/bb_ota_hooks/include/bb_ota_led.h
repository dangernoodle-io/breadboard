#pragma once
#include "bb_core.h"   // bb_ota_phase_t, bb_ota_progress_cb_t, bb_err_t
#ifdef __cplusplus
extern "C" {
#endif
// Action callbacks supplied by the consumer. The consumer OWNS LED rendering
// (its own bb_led semantics, colors, RGB-vs-single-channel). bb_ota_led owns the
// LIFECYCLE: `restore` is guaranteed to run on every terminal outcome that does
// NOT reboot (FAIL/abort), so a transient OTA indication can never latch.
typedef struct {
    void (*updating)(void *ctx, int pct); // START / PROGRESS: show "updating" (may be called repeatedly; be idempotent)
    void (*success)(void *ctx);           // SUCCESS: image written, reboot imminent (consumer may show "done")
    void (*restore)(void *ctx);           // terminal non-reboot (FAIL/abort): re-assert the steady base LED state
} bb_ota_led_ops_t;

// Register the action callbacks. `ops` must have static lifetime. `ctx` is opaque,
// passed back to each callback (may be NULL). Re-callable to replace.
void bb_ota_led_init(const bb_ota_led_ops_t *ops, void *ctx);

// A bb_ota_progress_cb_t to register with bb_ota_{pull,push,boot}_set_progress_cb().
// Routes phases to the registered ops and enforces the restore-on-terminal contract.
// No-op (safe) if bb_ota_led_init was never called or a given op is NULL.
void bb_ota_led_progress(bb_ota_phase_t phase, int pct);
#ifdef __cplusplus
}
#endif
