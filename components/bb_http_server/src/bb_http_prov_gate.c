#include "bb_http_prov_gate.h"
#include "bb_log.h"
#include "bb_route_match.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_http_prov_gate";

// One allowlist entry: (method, path). No precomputed wildcard metadata —
// bb_route_uri_match() detects the trailing '*' itself, so every entry
// (exact or wildcard) is matched the same way, through the one shared
// predicate. See bb_http_prov_gate.h for the /api/-only wildcard
// restriction and why matching is a union over entries, not a
// longest-prefix selection.
typedef struct {
    bb_http_method_t method;
    const char       *path;
} bb_http_prov_gate_entry_t;

// File-scope state — no heap, no ESP, s_ prefix per house rules.
static bb_http_prov_gate_entry_t s_allowlist[BB_HTTP_PROV_ALLOWLIST_CAP];
static size_t                    s_allowlist_count;

// Injected "is provisioning active" signal source (see bb_http_prov_gate.h).
// NULL (the default) means "never active" — an app that never wires a
// source sees zero gating.
static bb_http_prov_active_fn_t  s_active_fn;

void bb_http_prov_gate_reset(void)
{
    // TEST-ONLY (see header) — clear the count BEFORE zeroing the entries
    // (defence in depth): even though there is no production caller today,
    // this ordering means a hypothetical racing reader sees an empty table
    // rather than a stale count over zeroed (NULL-path) entries.
    s_allowlist_count = 0;
    memset(s_allowlist, 0, sizeof(s_allowlist));
    s_active_fn = NULL;
}

void bb_http_prov_gate_set_active_fn(bb_http_prov_active_fn_t fn)
{
    s_active_fn = fn;
}

bool bb_http_prov_gate_check(bb_http_method_t method, const char *uri)
{
    bool prov_active = s_active_fn ? s_active_fn() : false;
    return bb_http_prov_gate_allow(prov_active, method, uri);
}

bool bb_http_prov_gate_is_active(void)
{
#if defined(CONFIG_BB_WIFI_PROV_GATE_DISABLE) && CONFIG_BB_WIFI_PROV_GATE_DISABLE
    // Same escape hatch as bb_http_prov_gate_allow(): fully defeated, so
    // callers deciding whether to fail closed see "never active".
    return false;
#else
    return s_active_fn ? s_active_fn() : false;
#endif
}

bb_err_t bb_http_prov_allow(bb_http_method_t method, const char *path)
{
    if (!path) return BB_ERR_INVALID_ARG;

    bool is_wildcard = bb_route_match_is_wildcard(path);

    // INTERIM restriction (see header): a wildcard entry is only verified
    // correct for the /api/* family bb_route_uri_match's subset predicate
    // is actually exercised against (bb_dispatch_api). Reject a non-/api
    // wildcard rather than silently admitting an entry that could diverge
    // from what httpd actually routes.
    if (is_wildcard && !bb_route_match_wildcard_in_api_scope(path)) {
        bb_log_e(TAG, "wildcard prov-allow entry %s rejected: wildcard entries "
                 "are only permitted under /api/ (see bb_http_prov_gate.h)", path);
        return BB_ERR_INVALID_ARG;
    }

    if (s_allowlist_count >= BB_HTTP_PROV_ALLOWLIST_CAP) {
        bb_log_e(TAG, "prov allowlist full (cap=%d); dropping entry for %s",
                 BB_HTTP_PROV_ALLOWLIST_CAP, path);
        return BB_ERR_NO_SPACE;
    }

    s_allowlist[s_allowlist_count].method = method;
    s_allowlist[s_allowlist_count].path   = path;
    s_allowlist_count++;

    return BB_OK;
}

bool bb_http_prov_gate_allow(bool prov_active, bb_http_method_t method, const char *uri)
{
#if defined(CONFIG_BB_WIFI_PROV_GATE_DISABLE) && CONFIG_BB_WIFI_PROV_GATE_DISABLE
    // Build-time escape hatch: unconditionally allow. Debug/bring-up only —
    // see the Kconfig help text. This fully defeats the default-deny gate.
    (void)prov_active;
    (void)method;
    (void)uri;
    return true;
#else
    if (!prov_active) return true;
    if (!uri) return false;

    // Compute path length up to the first '?' (strip query string) — the
    // match_upto bound bb_route_uri_match compares against.
    size_t path_len = 0;
    while (uri[path_len] != '\0' && uri[path_len] != '?') {
        path_len++;
    }

    // Union over entries: any matching entry on the right method allows.
    for (size_t i = 0; i < s_allowlist_count; i++) {
        if (s_allowlist[i].method != method) continue;
        if (bb_route_uri_match(s_allowlist[i].path, uri, path_len)) return true;
    }

    return false;
#endif
}
