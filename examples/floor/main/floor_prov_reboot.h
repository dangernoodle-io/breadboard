#pragma once

// floor_prov_reboot — pure decision for floor's provisioning-is-transient
// reboot trigger (B1-809 review fix). examples/floor is a hand-wired
// composition root, not a component (see CLAUDE.md's composition-only
// model) -- floor_app.c's wifi_lifecycle_observer calls this function
// directly, and test/test_host/test_floor_prov_reboot.c #includes
// floor_prov_reboot.c directly (examples are not part of the native
// scaffold's component graph, so this is the only way to reach it from a
// host test) -- one code path, no mirror. See floor_app.c's
// wifi_lifecycle_observer doc for the full reboot rationale; this header
// only owns the guard's pure boolean logic.

#include <stdbool.h>
#include "bb_lifecycle.h"

// Decides whether the "wifi" lifecycle service's latest event should trigger
// floor's provisioning-is-transient reboot. All five guard branches are pure
// comparisons of the caller's own state -- no I/O, no globals.
//
//   prov_mode_boot    -- this boot started with no wifi creds
//                         (floor_app.c's s_prov_mode_boot)
//   already_triggered -- the reboot has already been queued this boot
//                         (floor_app.c's s_prov_reboot_triggered) -- a
//                         one-shot latch so a spurious re-fire of the
//                         RUNNING edge can never queue a second reboot
//   evt_svc           -- the service the fired event belongs to (evt->svc)
//   wifi_svc          -- floor's own "wifi" lifecycle service handle
//                         (floor_app.c's s_wifi_svc)
//   old_state/new_state -- the event's state transition
//
// Returns true only on a genuine STOPPED/PAUSED->RUNNING edge for wifi_svc,
// during a no-creds boot, not yet triggered this boot.
bool floor_should_trigger_prov_reboot(bool prov_mode_boot, bool already_triggered,
                                       bb_lifecycle_svc_t evt_svc, bb_lifecycle_svc_t wifi_svc,
                                       bb_lifecycle_state_t old_state, bb_lifecycle_state_t new_state);
