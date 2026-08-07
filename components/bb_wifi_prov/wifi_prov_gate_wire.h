#pragma once

#include "bb_http_server.h" // bb_http_asset_t, bb_err_t (via bb_core.h)

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// B1-1279 PR-4 -- registers bb_http_prov_gate's default-deny allowlist
// (components/bb_http_server/include/bb_http_prov_gate.h) with exactly the
// routes provisioning genuinely cannot complete without:
//
//   GET  /ping             project-wide liveness convention; bb_wifi_prov
//                          never registers this route itself, but policy
//                          requires it stay reachable during provisioning
//                          regardless of whether a consumer composes it.
//   POST /save             the provisioning form's submit target
//                          (bb_wifi_prov_start()'s prov_save_handler).
//   POST /api/wifi/scan    the SSID-list fetch the default portal page's own
//                          script issues on load (prov_default_form.html).
//   GET  <assets[i].path>  one entry per caller-supplied asset -- the
//                          portal page itself (assets MUST include a
//                          path="/" entry, see bb_wifi_prov_start()'s doc)
//                          plus any additional assets (css/js/images) a
//                          custom-branded portal serves.
//
// Deliberately does NOT allowlist bb_wifi_prov_start()'s optional extra()
// consumer routes (advanced-UI backing routes) -- the base provisioning
// flow above does not need them, and the gate's whole purpose is to keep
// anything not proven necessary unreachable while provisioning is active.
// A consumer whose custom assets genuinely depend on such a route must
// allowlist it itself via bb_http_prov_allow().
//
// The fixed three entries are registered BEFORE the per-asset loop, so if
// BB_HTTP_PROV_ALLOWLIST_CAP is ever too small for a composition (custom
// asset table plus the fixed entries), the fixed provisioning-required
// routes win the cap over decorative extra assets -- see bb_http_prov_
// allow()'s own BB_ERR_NO_SPACE handling (logs and drops the entry;
// default-deny still applies to whatever didn't fit).
//
// Pure C -- no ESP-IDF types beyond bb_http_asset_t (already portable, see
// bb_http_server.h) -- fully host-testable. The ESP-IDF shell
// (bb_wifi_prov_start(), platform/espidf/bb_wifi_prov/bb_wifi_prov.c) is the
// one production caller.
//
// assets may be NULL only when n == 0 (matches bb_http_register_assets'
// own contract). Returns BB_OK if every entry registered
// (fixed entries + all n assets); otherwise the FIRST non-BB_OK result from
// bb_http_prov_allow(), after still attempting every remaining entry
// (best-effort -- a rejected wildcard asset path, e.g., does not stop
// unrelated exact-path assets from still being registered).
//
// Records its own outcome for wifi_prov_gate_wire_last_registration_
// complete() below -- see that function's doc for why this exists.
bb_err_t wifi_prov_gate_wire_register(const bb_http_asset_t *assets, size_t n);

// Review fix (B1-1279 PR-4 finding): did the MOST RECENT
// wifi_prov_gate_wire_register() call register every entry it attempted?
// True after a call that returned BB_OK; false after one that returned
// non-BB_OK (e.g. BB_ERR_NO_SPACE when a composition's fixed(3)+n entries
// exceed BB_HTTP_PROV_ALLOWLIST_CAP). True (vacuously) if
// wifi_prov_gate_wire_register() has never been called.
//
// This exists as a SEPARATELY queryable seam specifically because
// wifi_prov_gate_wire_register()'s bb_err_t return is easy to discard at
// the one production call site (bb_wifi_prov_start(), which -- pre-fix --
// only logged a non-BB_OK result and still returned BB_OK itself). A denied
// route fails closed either way (safe), but a dropped asset with no other
// signal 404s silently for the life of the process. bb_wifi_prov exposes
// this via bb_wifi_prov_gate_degraded() (see bb_wifi_prov.h) so a consumer
// can detect and report the degradation without bb_wifi_prov_start() itself
// having to fail (and abort provisioning) over what is a build-time
// composition mistake, not a security regression -- the allowlist still
// fails closed on whatever didn't fit.
bool wifi_prov_gate_wire_last_registration_complete(void);

// TEST-ONLY -- resets wifi_prov_gate_wire_last_registration_complete() back
// to its vacuous initial state (true). No production caller; s_gate_wired
// in bb_wifi_prov.c already means the one production call site only ever
// calls wifi_prov_gate_wire_register() once per process lifetime, so this
// exists purely for host-test isolation (mirrors bb_http_prov_gate_reset()).
void wifi_prov_gate_wire_reset_for_test(void);

#ifdef __cplusplus
}
#endif
