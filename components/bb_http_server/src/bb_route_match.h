#pragma once

// bb_route_uri_match — the ONE pattern-vs-URI predicate seam in
// bb_http_server. bb_http_server-internal (src-private, not in include/) —
// no consumer outside this component's own .c files.
//
// One function, one job: given a route pattern and a request URI, does the
// pattern match, comparing only the first `match_upto` bytes of `uri`.
// Nothing else is folded in here — no method comparison, no entry
// selection/priority (longest-prefix, exact-beats-wildcard), no table
// walking; those stay in each caller (route_registry.c's bb_dispatch_api_*,
// bb_http_prov_gate.c's allowlist walk). Every pattern-matching call site in
// this component funnels through this one function so a correctness fix
// lands once, not N times.
//
// INTERIM implementation (this is a narrow, deliberate subset): supports
// only a single trailing '*' suffix wildcard (prefix match) or an exact
// match otherwise. It does NOT support interior wildcards or '?' single-
// char patterns. No route in this tree currently needs those, but "no
// consumer today" is not a contract — the TARGET shape below is what closes
// that gap, not a documentation promise about current callers.
//
// TARGET SHAPE (not built in this PR): this seam is meant to become a
// PLATFORM HOOK, not a shared/vendored implementation:
//   - ESP-IDF: delegates directly to esp_http_server's own
//     httpd_uri_match_wildcard(uri_template, uri_to_match, match_upto) — the
//     actual matcher httpd itself uses to route native (non-/api) handlers,
//     so breadboard carries ZERO matching semantics of its own on-device.
//   - Host: a deliberately TRIVIAL stub sufficient only to exercise
//     breadboard's own entry-selection/priority logic (which entry wins,
//     longest-prefix-wins, exact-beats-wildcard, etc) in host tests. It is
//     NOT an attempt to reproduce httpd_uri_match_wildcard's full glob
//     semantics and never will be — host tests can only validate SELECTION
//     logic and exact/simple-prefix patterns; anything depending on richer
//     wildcard semantics (interior '*', '?') is only validly testable
//     on-device against the real httpd matcher.
//
// The signature below is already argument-compatible with
// httpd_uri_match_wildcard's (uri_template, uri_to_match, match_upto) shape
// specifically so that future platform-hook swap is a body replacement with
// NO caller churn: `match_upto` bounds how many bytes of `uri` participate
// in the match (callers pass the query-stripped length), exactly like
// httpd's own match_upto parameter — it is NOT a query-string-stripping
// helper's job here, callers own that.
//
// Any caller (or test) relying on subset-only behavior — interior wildcards
// or '?' patterns not supported by this interim implementation — is relying
// on something temporary; write those against the real device matcher, not
// here.

#include <stddef.h>
#include <stdbool.h>

bool bb_route_uri_match(const char *pattern, const char *uri, size_t match_upto);

// ---------------------------------------------------------------------------
// Shared entry-admission idiom -- consolidated out of bb_route_exclude.c and
// bb_http_prov_gate.c (both hand-rolled the identical wildcard-detection +
// /api/-only-wildcard restriction before this extraction; second instance
// triggers consolidation per house rule). Only the parts that were BYTE-
// IDENTICAL between the two call sites live here: NUL-termination handling,
// caller-specific logging, error codes, empty-path policy, and cap-exhaustion
// checks stay in each call site because they differ (or are allowed to
// differ) between bb_http_route_exclude() and bb_http_prov_allow().
//
// KNOWN DIVERGENCE (B1-1405, deliberate -- not accidental drift): an empty
// path is rejected by bb_http_route_exclude() but accepted by
// bb_http_prov_allow() as a non-wildcard exact entry (see
// test_bb_http_prov_gate_empty_path_not_wildcard). Do not "fix" one call
// site to match the other without re-checking B1-1405 first.
// ---------------------------------------------------------------------------

// Does `path` look like a suffix-wildcard entry (trailing '*')? path must be
// non-NULL -- callers reject NULL before calling this.
bool bb_route_match_is_wildcard(const char *path);

// Is a wildcard `path` inside the INTERIM /api/-only wildcard scope (see
// bb_route_match.h's TARGET SHAPE note above, and each caller's own header
// for why the restriction exists)? path must be non-NULL. The two callers
// use this differently: bb_http_prov_gate.c gates the call behind
// bb_route_match_is_wildcard(path) (only asks when path is already known to
// be a wildcard); bb_http_route_exclude() calls it unconditionally, on every
// path, as a general /api/-scope check (the strncmp(path, "/api/", 5)
// body is correct either way).
bool bb_route_match_wildcard_in_api_scope(const char *path);
