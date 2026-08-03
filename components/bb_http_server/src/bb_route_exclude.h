#pragma once

// bb_route_exclude — src-private seam for route_registry.c only (B1-1401).
//
// route_registry.c's registry_add() and bb_dispatch_api_add() call
// route_excluded() at each of their insertion points to check a
// (method, path) route against the composition-time exclusion table
// before storing it. The PUBLIC entry points a consumer calls to
// populate and verify that table -- bb_http_route_exclude(),
// bb_http_route_exclude_verify(), bb_http_route_exclude_reset() -- are
// declared in bb_http_server.h (this component's real public surface,
// see its doc comments for the full contract and scope limits) and
// defined in bb_route_exclude.c alongside this file's table. This
// header exists only to give route_registry.c the internal
// route_excluded() call without carving out a second component-public
// header for what is a single-consumer seam.

#include "bb_http.h"

#include <stdbool.h>

// Maximum number of (method,path) exclusion entries the table holds.
// Kconfig bridge, not a bare #ifndef shadow of the generated symbol --
// same style as BB_HTTP_PROV_ALLOWLIST_CAP (bb_http_prov_gate.h).
#ifdef CONFIG_BB_HTTP_ROUTE_EXCLUDE_CAP
#define BB_HTTP_ROUTE_EXCLUDE_CAP CONFIG_BB_HTTP_ROUTE_EXCLUDE_CAP
#else
#define BB_HTTP_ROUTE_EXCLUDE_CAP 8
#endif

// Internal helper: does (method, path) match an entry in the exclusion
// table? On a hit, marks that entry's `matched` flag true (consumed by
// bb_http_route_exclude_verify()'s typo guard) and returns true --
// route_registry.c's two insertion points (registry_add(),
// bb_dispatch_api_add()) use this to skip storing the route as a
// benign no-op rather than a registration failure. path may be NULL
// (matches nothing).
bool route_excluded(bb_http_method_t method, const char *path);
