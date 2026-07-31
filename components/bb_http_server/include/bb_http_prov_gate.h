#pragma once

// bb_http_prov_gate — deny-by-default route allowlist for WiFi provisioning.
//
// During provisioning the SoftAP uses a fixed default password, so every
// reachable route is effectively public to anyone in radio range. This
// primitive gives the HTTP dispatch path a structural default-deny posture
// while provisioning is active: a route is reachable ONLY if a consumer has
// explicitly named it via bb_http_prov_allow(). There is deliberately no
// "allow all" runtime bit.
//
// PORTABLE C — no ESP-IDF headers, no httpd types. bb_http_prov_gate_allow()
// takes prov_active as a parameter so this file has zero dependency on
// bb_wifi_prov (or any provisioning-state source) and is fully host-testable.
//
// PR-3 of a multi-PR stream (B1-1279) wires this gate into the HTTP dispatch
// path: bb_http_prov_gate_check() below is the single decision function every
// dispatch insertion point calls (see platform/espidf/bb_http_server/bb_http.c
// and its host mirror, platform/host/bb_http_server/bb_http_host.c). Actually
// registering the provisioning endpoints and /ping via bb_http_prov_allow()
// is the consumer's job (bb_wifi_prov, app composition) — this component
// only ships the mechanism.
//
// Matching: entry selection here is a UNION over the allowlist (any entry
// on the right method that matches wins) — deliberately NOT
// bb_dispatch_api_lookup's two-pass exact-beats-wildcard /
// longest-prefix-wins selection. Union is the correct rule for an
// allowlist: a narrower wildcard entry must never suppress a broader one
// that would also have allowed the request. What IS shared with
// bb_dispatch_api is the per-entry exact/suffix-wildcard PREDICATE itself
// (bb_route_uri_match, see components/bb_http_server/src/bb_route_match.h)
// — the selection strategy layered on top of it differs on purpose.
//
// INTERIM restriction (removable): bb_http_prov_allow() only permits a
// wildcard ('*') entry for a path under /api/. bb_route_uri_match's
// wildcard predicate is verified correct only for the /api/* family
// actually dispatched through bb_dispatch_api — non-/api routes (captive
// portal pages, the asset GET /*, /ping) are registered as native
// httpd_uri_t and matched by ESP-IDF's own httpd_uri_match_wildcard, a
// third matcher this gate never touches. A wildcard allowlist entry for a
// non-/api path could silently diverge from what httpd actually routes
// (false-deny self-DoS, or a gate/dispatcher mismatch the other way). This
// restriction goes away once bb_route_uri_match itself delegates to the
// platform's real matcher (see bb_route_match.h's TARGET SHAPE) instead of
// carrying its own interim subset — at that point a wildcard entry means
// the same thing everywhere and the /api/-only rule is no longer needed.

#include "bb_http.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of (method,path) entries the allowlist holds. Deliberately
// small — this list is meant to stay short (provisioning endpoints + /ping).
#ifdef CONFIG_BB_HTTP_PROV_ALLOWLIST_CAP
#define BB_HTTP_PROV_ALLOWLIST_CAP CONFIG_BB_HTTP_PROV_ALLOWLIST_CAP
#else
#define BB_HTTP_PROV_ALLOWLIST_CAP 8
#endif

// Record that (method, path) is reachable while provisioning is active.
// `path` must have static/persistent storage duration — only the pointer is
// stored. Safe to call for a path never registered elsewhere.
//
// A path ending in '*' is a suffix-wildcard (prefix) entry; see the
// INTERIM restriction above — a wildcard entry is accepted ONLY when path
// starts with "/api/"; a non-/api wildcard is rejected with
// BB_ERR_INVALID_ARG (and logged) rather than silently admitted. Any other
// path is an exact-match entry with no such restriction.
//
// Returns BB_OK on success, BB_ERR_INVALID_ARG for a NULL path or a
// rejected non-/api wildcard, or BB_ERR_NO_SPACE when the allowlist is full
// (BB_HTTP_PROV_ALLOWLIST_CAP entries already recorded).
bb_err_t bb_http_prov_allow(bb_http_method_t method, const char *path);

// Pure decision function: is (method, uri) reachable right now?
//
// When prov_active is false, always returns true (provisioning isn't
// gating anything). When prov_active is true, returns true only if
// (method, uri) matches an entry previously recorded via
// bb_http_prov_allow() — an allowlisted path under the WRONG method still
// returns false (the allowlist is per (method,path), not per path). No
// allowlist entry means denied; this is structural, there is no override.
bool bb_http_prov_gate_allow(bool prov_active, bb_http_method_t method, const char *uri);

// Signal source: does the caller currently consider provisioning active?
// bb_http_prov_gate itself has zero dependency on bb_wifi_prov (or any other
// provisioning-state source, see the file header) — it learns "is
// provisioning active" only through this injected function pointer, never by
// calling a provisioning component directly. This is what lets
// bb_http_server ship the gate without REQUIRES'ing bb_wifi_prov, which
// itself REQUIRES bb_http_server (a direct dependency the other way would be
// a build-time cycle).
typedef bool (*bb_http_prov_active_fn_t)(void);

// Register the function bb_http_prov_gate_check() calls to resolve
// prov_active. This is production wiring, not a test hook — e.g. app
// composition code (handwire or codegen) calls
// bb_http_prov_gate_set_active_fn(bb_wifi_prov_is_active) once at boot when
// it composes the provisioning portal. Passing NULL (also the default,
// never-set state) makes bb_http_prov_gate_check() always resolve
// prov_active to false — an app that never wires a signal source sees zero
// gating, identical to pre-PR-3 behavior.
void bb_http_prov_gate_set_active_fn(bb_http_prov_active_fn_t fn);

// Combined check: resolves prov_active via the function registered with
// bb_http_prov_gate_set_active_fn() (false if none registered), then returns
// bb_http_prov_gate_allow(prov_active, method, uri). This is the ONE
// function every HTTP dispatch insertion point calls to decide whether a
// request may proceed — see bb_http.c's api_dispatch_handler,
// bb_shim_handler, and asset_wildcard_handler (and their host mirrors in
// bb_http_host.c) for the call sites.
bool bb_http_prov_gate_check(bb_http_method_t method, const char *uri);

// Resolves prov_active via the function registered with
// bb_http_prov_gate_set_active_fn() (false if none registered), same as
// bb_http_prov_gate_check() — but with no (method, uri) to evaluate against
// the allowlist. For call sites that must decide whether to fail closed on
// their own terms rather than through an allowlist match, e.g.
// method_not_allowed_err_handler's unmappable-HTTP-method branch (HEAD,
// etc. — bb_http_method_from_httpd() only maps a fixed subset, so those
// requests have no bb_http_method_t to pass to bb_http_prov_gate_check() at
// all). Honors CONFIG_BB_WIFI_PROV_GATE_DISABLE the same way
// bb_http_prov_gate_allow() does (always false when the escape hatch is on).
bool bb_http_prov_gate_is_active(void);

// Clear the allowlist and the injected active-fn. TEST-ONLY — used by
// test_main.c's setUp() to isolate the process-wide static state between
// tests. NOT safe to call with httpd workers live (no locking): a worker
// mid-bb_http_prov_gate_allow() can observe a torn state between this
// clearing the count and zeroing the entries. There is no production caller
// of this function anywhere in-tree (same convention as
// bb_dispatch_api_reset() / bb_http_route_registry_clear() in
// route_registry.c).
void bb_http_prov_gate_reset(void);

#ifdef __cplusplus
}
#endif
