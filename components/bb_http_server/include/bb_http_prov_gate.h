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
//
// DENY (B1-1400): bb_http_prov_deny() records narrow carve-outs the
// allowlist should NOT permit even though a broader allow entry matches --
// e.g. allowing /api/diag/* while still denying /api/diag/reboot
// specifically (the B1-1279 motivating case). Deny is a STRICT POST-FILTER
// evaluated AFTER the union allowlist resolves, never mixed into the same
// pass, and never given any precedence/longest-prefix scheme of its own --
// that would break the allow-vs-allow order-independence guaranteed above.
// The resulting predicate is: union(allow matches) && !union(deny
// matches). Deny always wins over allow; there is no way to un-deny a path
// short of not calling bb_http_prov_deny() for it. Denylist entries are
// stored in a SEPARATE table from the allowlist (same entry shape, same
// per-(method,path) granularity, same /api/-only wildcard restriction as
// bb_http_prov_allow() -- see bb_http_prov_deny() below).
//
// Interaction with bb_http_route_exclude() (B1-1401, a different
// component-internal primitive, see bb_route_exclude.h): a route excluded
// via bb_http_route_exclude() is never registered at all --
// bb_dispatch_api_lookup() misses and the request 404s before dispatch
// ever calls this gate. This gate is simply never reached for an excluded
// path. There is no interaction logic between deny and exclude beyond
// that -- a deny entry for an already-excluded path is dead but harmless
// (it names a path the gate will never be asked to decide). Layer
// distinction: exclude is permanent/structural/always-404 (composition
// time, never provisioning-scoped); deny is a runtime,
// provisioning-window-scoped restriction only -- the path works normally
// once provisioning ends.

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

// Maximum number of (method,path) entries the denylist holds. Deliberately
// small — this list is meant to stay short (narrow carve-outs under a
// broader allow entry, see the DENY note above).
#ifdef CONFIG_BB_HTTP_PROV_DENYLIST_CAP
#define BB_HTTP_PROV_DENYLIST_CAP CONFIG_BB_HTTP_PROV_DENYLIST_CAP
#else
#define BB_HTTP_PROV_DENYLIST_CAP 4
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
// (BB_HTTP_PROV_ALLOWLIST_CAP entries already recorded). A dropped ALLOW
// entry is safe: this gate's default posture is deny, so a path that never
// made it into the allowlist simply stays denied — overflow here costs the
// caller functionality (a route it wanted reachable during provisioning
// isn't), never security. Contrast bb_http_prov_deny() below, whose
// overflow is the opposite direction.
bb_err_t bb_http_prov_allow(bb_http_method_t method, const char *path);

// Record that (method, path) is DENIED while provisioning is active, even
// if a broader bb_http_prov_allow() entry would otherwise match it. See
// the DENY note above for the strict-post-filter semantics: deny always
// wins over allow. `path` must have static/persistent storage duration —
// only the pointer is stored. Safe to call for a path never registered via
// bb_http_prov_allow() (it simply never matters — deny only ever narrows
// an allow, it can't itself grant access).
//
// Same wildcard restriction as bb_http_prov_allow(): a wildcard ('*') entry
// is accepted ONLY when path starts with "/api/"; a non-/api wildcard is
// rejected with BB_ERR_INVALID_ARG. A non-wildcard path outside /api/ is
// permitted (deny is not /api/-restricted overall, only wildcard deny
// entries are, exactly mirroring allow).
//
// An empty path is accepted as a non-wildcard exact entry, the same
// precedent bb_http_prov_allow() sets (NOT bb_http_route_exclude()'s
// rejection of an empty path — see B1-1405, bb_route_match.h's KNOWN
// DIVERGENCE note). Deny is allow's direct sibling primitive; exclude is a
// different layer with a different empty-path failure mode.
//
// Deny is per-(method,path) with no "all methods" form, exactly mirroring
// bb_http_prov_allow() — not a bug, but it means a caller trying to carve
// a path out entirely must call this once per method that has a matching
// allow entry; a partially-denied method set is indistinguishable from a
// fully-denied one at the call site (bb_http_prov_gate_allow() only ever
// sees one method per request) and there is no verify_denies()-style check
// for a forgotten method.
//
// Returns BB_OK on success, BB_ERR_INVALID_ARG for a NULL path or a
// rejected non-/api wildcard, or BB_ERR_NO_SPACE when the denylist is full
// (BB_HTTP_PROV_DENYLIST_CAP entries already recorded). UNLIKE
// bb_http_prov_allow()'s overflow above, a dropped DENY entry is NOT
// safe: the entry that failed to record is exactly the carve-out meant to
// narrow an already-allowed path, so its absence leaves that path
// ALLOWED — this fails OPEN, the opposite direction from allow's
// fail-closed overflow, and bb_http_prov_gate_verify_denies() cannot catch
// it either (a dropped entry was never recorded, so there is nothing to
// verify). A caller MUST treat any non-BB_OK return from this function as
// fatal (abort composition / refuse to boot into provisioning) rather than
// best-effort — do NOT copy bb_http_prov_allow()'s "a dropped entry is
// safe" assumption across to this function.
bb_err_t bb_http_prov_deny(bb_http_method_t method, const char *path);

// Pure decision function: is (method, uri) reachable right now?
//
// When prov_active is false, always returns true (provisioning isn't
// gating anything). When prov_active is true: first resolves the union
// allowlist exactly as before (returns true only if (method, uri) matches
// an entry previously recorded via bb_http_prov_allow() — an allowlisted
// path under the WRONG method still returns false, the allowlist is per
// (method,path), not per path). If the allowlist denies, this returns
// false immediately, unchanged from pre-deny behavior. If the allowlist
// allows, the denylist is then consulted as a strict post-filter: any
// matching bb_http_prov_deny() entry on the same method flips the result
// to false. No allowlist entry (or a matching deny entry) means denied;
// this is structural, there is no override.
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

// Composition-time static reachability check for the denylist: for every
// entry recorded via bb_http_prov_deny(), is there ANY allow entry that
// could ever match the same (method, path)? An orphaned deny entry — one
// no allow entry could ever reach — is very likely a typo or a stale
// carve-out left over after the allow entry it was meant to narrow was
// removed; it currently does nothing (the strict post-filter in
// bb_http_prov_gate_allow() only ever runs after an allow match, so an
// unreachable deny entry is silently inert, not a hidden extra
// restriction).
//
// This is a STATIC config-vs-config check, deliberately NOT a per-request
// "matched" latch (contrast bb_http_route_exclude_verify()'s B1-1401
// design, which DOES use a matched latch): a deny entry is evaluated at
// REQUEST time against live URIs, so a latch would false-positive on every
// cold boot before the first matching request ever arrives — there is no
// way to distinguish "unreachable" from "reachable but not yet exercised"
// with a latch. Reachability between two static tables (allow, deny) is
// fully decidable at composition time by feeding each deny entry's path
// through bb_route_uri_match() against every allow entry, with no
// per-request bookkeeping required.
//
// SCOPE LIMIT: this only catches "a deny entry names a path no allow entry
// could ever reach". It does NOT catch the opposite mistake — "a path that
// should have been denied but the app forgot to call bb_http_prov_deny()
// for it" — that omission has no static signature to detect; it is a
// review-time concern, not an automatable one.
//
// Call once at composition time (after all bb_http_prov_allow() and
// bb_http_prov_deny() calls have run, before the HTTP server starts
// serving). Returns BB_OK if every deny entry is reachable by some allow
// entry, or BB_ERR_NOT_FOUND if ANY orphan exists. The return value itself
// does not identify which entry (or entries) are orphaned — the bare
// bb_err_t carries no out-param for that. EVERY orphan found is logged (not
// just the first); read the log, not the return value, to find which
// entries need fixing.
bb_err_t bb_http_prov_gate_verify_denies(void);

// Clear the allowlist, the denylist, and the injected active-fn. TEST-ONLY
// — used by test_main.c's setUp() to isolate the process-wide static state
// between tests. NOT safe to call with httpd workers live (no locking): a
// worker mid-bb_http_prov_gate_allow() can observe a torn state between
// this clearing a count and zeroing its entries. There is no production
// caller of this function anywhere in-tree (same convention as
// bb_dispatch_api_reset() / bb_http_route_registry_clear() in
// route_registry.c).
void bb_http_prov_gate_reset(void);

#ifdef __cplusplus
}
#endif
