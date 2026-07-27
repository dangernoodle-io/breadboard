#pragma once

// wifi_prov_mgr -- ESP-IDF provisioning manager task shell (B1-809 PR3b).
// Owns the queue + task that drives the wifi_prov_policy.h FSM
// (wifi_prov_ctx_t / wp_state_t / wp_event_t). Private to bb_wifi_prov --
// not part of the public bb_wifi_prov.h surface; PR4 adds the public entry
// points (boot-time first-boot/deprov-anomaly checks, user-requested
// re-provision route) that call wifi_prov_mgr_init()/_post_entry().
//
// LANDS INERT in PR3b: nothing outside this component calls
// wifi_prov_mgr_init() yet, so s_queue stays NULL and
// wifi_prov_mgr_on_save() (bb_wifi_prov.c's /save touch) takes its
// `if (!s_queue) return;` early-out on every call.

#include <stddef.h>

#include "bb_core.h"
#include "bb_wifi_prov.h"   // bb_http_asset_t, bb_wifi_prov_extra_routes_fn_t

// Create the manager's queue + FSM ctx (wifi_prov_fsm_init, WP_IDLE) + task
// (bb_task_create). Idempotent: a second call while already initialized is
// a no-op (mirrors wifi_reconn_start's s_active guard). `assets`/`n`/`extra`
// are stashed in file-static state and closed over by the adapter's
// zero-arg prov_start_fn (wifi_prov_policy.h's adapter contract -- bb_http_
// asset_t must not leak into that pure header).
bb_err_t wifi_prov_mgr_init(const bb_http_asset_t *assets, size_t n,
                             bb_wifi_prov_extra_routes_fn_t extra);

// Non-blocking notifier: post EV_SAVE_SUCCESS to the manager's queue and
// return. Called by bb_wifi_prov.c's prov_save_handler, on the HTTP task,
// as the LAST statement of the /save success path (after
// bb_wifi_prov_signal_done()) so the HTTP response is fully written before
// the manager can begin teardown. No-op if the manager isn't running
// (s_queue unset) -- this is what keeps this PR inert.
void wifi_prov_mgr_on_save(void);
