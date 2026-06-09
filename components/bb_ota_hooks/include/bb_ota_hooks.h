#pragma once
#include "bb_core.h"   // bb_http_pause/resume_cb_t, bb_ota_progress_cb_t, bb_ota_skip_check_cb_t, bb_ota_phase_t, bb_err_t
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// --- Consumer sets these ONCE; shared by all OTA paths (push/pull/boot) ---
void bb_ota_set_hooks(bb_http_pause_cb_t pause, bb_http_resume_cb_t resume);
void bb_ota_set_progress_cb(bb_ota_progress_cb_t cb);
void bb_ota_set_skip_check_cb(bb_ota_skip_check_cb_t cb);

// --- Producer API: bb_ota_push/pull/boot call these (NOT consumer-facing) ---
bool bb_ota_pause(void);       // s_pause_cb ? s_pause_cb() : false
bool bb_ota_has_pause_hook(void);  // true if a pause hook is installed (lets callers distinguish "no hook" from "hook returned false")
void bb_ota_resume(void);      // if (s_resume_cb) s_resume_cb()
bool bb_ota_skip_check(void);  // s_skip_check_cb && s_skip_check_cb()
// Fire the consumer progress cb + serial log + last-progress stash + (esp) ota.progress SSE.
// via is "push"|"pull"|"boot".
void bb_ota_emit_progress(const char *via, bb_ota_phase_t phase, int pct);
// Read the last emitted progress (for boot's GET /api/update/progress route).
void bb_ota_last_progress(bb_ota_phase_t *phase, int *pct);

// --- Pure, host-tested ---
// Build the ota.progress JSON. Returns bytes written (excl NUL), 0 on overflow.
// state: 0=start,1=progress,2=success,3=fail (>3 -> "unknown").
int bb_ota_progress_json(char *buf, size_t sz, const char *via, int state, int pct);

#ifdef __cplusplus
}
#endif
