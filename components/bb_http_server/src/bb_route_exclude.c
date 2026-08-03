#include "bb_route_exclude.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_route_match.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_http_route_exclude";

// One exclusion-table entry: (method, path) plus the typo-guard latch
// consumed by bb_http_route_exclude_verify(). No precomputed wildcard
// metadata -- bb_route_uri_match() detects the trailing '*' itself, same
// convention as bb_http_prov_gate.c's allowlist entry.
typedef struct {
    bb_http_method_t method;
    const char       *path;
    bool              matched;
} bb_http_route_exclude_entry_t;

// File-scope state -- no heap, no ESP, s_ prefix per house rules.
static bb_http_route_exclude_entry_t s_exclude[BB_HTTP_ROUTE_EXCLUDE_CAP];
static size_t                        s_exclude_count;

void bb_http_route_exclude_reset(void)
{
    memset(s_exclude, 0, sizeof(s_exclude));
    s_exclude_count = 0;
}

bb_err_t bb_http_route_exclude(bb_http_method_t method, const char *path)
{
    // Empty path is the same class of caller error as NULL: it can never
    // match anything (bb_route_uri_match's exact-match branch requires
    // plen == match_upto, and a real registered route path is never empty),
    // so admitting it would just consume a table slot destined to fail
    // bb_http_route_exclude_verify()'s typo guard with a confusing "never
    // matched" instead of failing loudly here.
    if (!path || !path[0]) return BB_ERR_INVALID_ARG;

    // B1-1401 HIGH fix: this primitive is enforced ONLY inside registry_add()
    // and bb_dispatch_api_add() (route_registry.c) -- the /api/ choke points.
    // A handler-bound route reaches bb_http_register_route() FIRST (both
    // backends register the real handler unconditionally, with no knowledge
    // of this table); registry_add() runs only afterward. For a non-/api/
    // path there is no equivalent choke point, so admitting the entry would
    // return BB_OK while doing nothing -- the exact silent-failure mode
    // bb_http_route_exclude_verify()'s typo guard exists to catch, and it
    // can't catch this because the entry never gets a chance to "not match":
    // it is simply never consulted. Reject every non-/api/ path outright
    // (exact or wildcard alike -- this supersedes the narrower wildcard-only
    // restriction bb_http_prov_allow() still applies) rather than admit an
    // entry that can never actually gate anything.
    if (!bb_route_match_wildcard_in_api_scope(path)) {
        bb_log_e(TAG, "route-exclude entry %s rejected: only /api/ paths "
                 "(exact or wildcard) are supported -- non-/api/ routes are "
                 "registered directly by the platform backend before this "
                 "exclusion table is consulted, so excluding one here would "
                 "silently do nothing", path);
        return BB_ERR_INVALID_ARG;
    }

    if (s_exclude_count >= BB_HTTP_ROUTE_EXCLUDE_CAP) {
        bb_log_e(TAG, "route-exclude table full (cap=%d); dropping entry for %s",
                 BB_HTTP_ROUTE_EXCLUDE_CAP, path);
        return BB_ERR_NO_SPACE;
    }

    s_exclude[s_exclude_count].method  = method;
    s_exclude[s_exclude_count].path    = path;
    s_exclude[s_exclude_count].matched = false;
    s_exclude_count++;

    return BB_OK;
}

bool route_excluded(bb_http_method_t method, const char *path)
{
    if (!path) return false;

    size_t path_len = strlen(path);

    for (size_t i = 0; i < s_exclude_count; i++) {
        if (s_exclude[i].method != method) continue;
        if (!bb_route_uri_match(s_exclude[i].path, path, path_len)) continue;
        s_exclude[i].matched = true;
        return true;
    }
    return false;
}

bb_err_t bb_http_route_exclude_verify(void)
{
    for (size_t i = 0; i < s_exclude_count; i++) {
        if (!s_exclude[i].matched) {
            // No NULL-path ternary here (unlike route_registry.c's overflow
            // logs): bb_http_route_exclude() rejects a NULL path up front
            // (BB_ERR_INVALID_ARG), so every stored entry's path is always
            // non-NULL.
            bb_log_e(TAG, "route-exclude entry never matched a registration: "
                     "method=%d path=%s",
                     (int)s_exclude[i].method, s_exclude[i].path);
            return BB_ERR_NOT_FOUND;
        }
    }
    return BB_OK;
}
