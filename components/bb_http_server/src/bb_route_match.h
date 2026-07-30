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
