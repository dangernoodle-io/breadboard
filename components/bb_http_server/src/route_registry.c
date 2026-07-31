#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_route_match.h"
#include "bb_str.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Maximum number of described routes the registry holds.
// Overflow is logged and the descriptor is silently dropped.
#ifdef CONFIG_BB_HTTP_ROUTE_REGISTRY_CAP
#define BB_ROUTE_REGISTRY_CAP CONFIG_BB_HTTP_ROUTE_REGISTRY_CAP
#else
#define BB_ROUTE_REGISTRY_CAP 64
#endif

// Elevate a registry-overflow drop to a fatal assert (see registry_add()).
// Kconfig bridge, not a bare #ifndef shadow of the generated symbol.
#ifdef CONFIG_BB_HTTP_ROUTE_REGISTRY_STRICT
#define BB_HTTP_ROUTE_REGISTRY_STRICT CONFIG_BB_HTTP_ROUTE_REGISTRY_STRICT
#else
#define BB_HTTP_ROUTE_REGISTRY_STRICT 1
#endif

static const char *TAG = "bb_http_registry";

// Process-wide static registry.  Stores pointers only — descriptor
// lifetime is the caller's responsibility (same convention as bb_http_asset_t).
static const bb_route_t *s_registry[BB_ROUTE_REGISTRY_CAP];
static size_t             s_count = 0;
static bool                s_watermark_warned = false;

// ---------------------------------------------------------------------------
// Host-only test seams (B1-1319 review): structurally excluded from every
// production build via #ifdef BB_HTTP_TESTING (defined only by
// [env:native]'s build_flags, platformio.ini) -- NOT relying on
// --gc-sections to strip them, which is incidental dead-code elimination,
// not a compile-time guarantee, and regresses the moment any production
// call site references one. route_registry.c is in
// components/bb_http_server/CMakeLists.txt's unconditional SRCS (unlike
// bb_http_host_force_register_fail(), which lives in the host-only TU
// platform/host/bb_http_server/bb_http_host.c and is never linked into
// device firmware) -- the setter/reset function SHAPE mirrors that
// precedent, but the PLACEMENT does not, hence the explicit guard here
// instead of relying on file location to keep it off-device.
// ---------------------------------------------------------------------------

#ifdef BB_HTTP_TESTING
// Host-testable override for the STRICT decision: the compile-time
// BB_HTTP_ROUTE_REGISTRY_STRICT default is fixed per build, so without this
// override a host test could only ever exercise the true branch of
// registry_add()'s "if (bb_http_route_registry_strict_would_fire())" --
// this lets a test flip it to false and prove the escalation (and its trap
// hook below) does NOT run when STRICT is off. Not a public API — declared
// extern in test_route_registry.c only.
static bool s_strict_override_active = false;
static bool s_strict_override_value  = false;

void bb_http_route_registry_strict_force(bool enabled)
{
    s_strict_override_active = true;
    s_strict_override_value  = enabled;
}

void bb_http_route_registry_strict_force_reset(void)
{
    s_strict_override_active = false;
}
#endif // BB_HTTP_TESTING

bool bb_http_route_registry_strict_would_fire(void)
{
#ifdef BB_HTTP_TESTING
    if (s_strict_override_active) return s_strict_override_value;
#endif
    return (bool)BB_HTTP_ROUTE_REGISTRY_STRICT;
}

#ifdef BB_HTTP_TESTING
// Host-testable trap hook: set the instant registry_add() actually ENTERS
// the overflow+STRICT escalation branch, before the (also
// BB_HTTP_TESTING-compiled-out) assert(). bb_http_route_registry_strict_
// would_fire() alone only proves the Kconfig default resolves to true in
// isolation -- it says nothing about whether that predicate is actually
// wired into the overflow path. This trap does: a host test drives a real
// overflow through registry_add() and asserts the trap fired, so deleting
// or short-circuiting the escalation block in registry_add() fails the
// test. Not a public API — declared extern in test_route_registry.c only.
static bool s_strict_trap_hit = false;

bool bb_http_route_registry_strict_trap_hit(void)
{
    return s_strict_trap_hit;
}

void bb_http_route_registry_strict_trap_reset(void)
{
    s_strict_trap_hit = false;
}
#endif // BB_HTTP_TESTING

// ---------------------------------------------------------------------------
// Public registry API
// ---------------------------------------------------------------------------

void bb_http_route_registry_clear(void)
{
    for (size_t i = 0; i < s_count; i++) {
        s_registry[i] = NULL;
    }
    s_count = 0;
    s_watermark_warned = false;
#ifdef BB_HTTP_TESTING
    // Reset the host-only test seams too, so a test that forgets an
    // explicit _trap_reset()/_force_reset() call still starts clean --
    // clear() is already the universal per-test setup call.
    s_strict_trap_hit         = false;
    s_strict_override_active  = false;
    s_strict_override_value   = false;
#endif
}

size_t bb_http_route_registry_count(void)
{
    return s_count;
}

void bb_http_route_registry_foreach(bb_route_walker_fn cb, void *ctx)
{
    if (!cb) return;
    for (size_t i = 0; i < s_count; i++) {
        cb(s_registry[i], ctx);
    }
}

// ---------------------------------------------------------------------------
// Internal helper: add a descriptor pointer to the registry
// ---------------------------------------------------------------------------

// IMPORTANT: callers MUST pass a descriptor with static or persistent storage
// duration.  A stack-local bb_route_t dangles immediately after the enclosing
// function returns; the 405 walk dereferences r->path on every request and will
// LoadProhibited-crash on the garbage pointer.  Use a function-static or
// translation-unit-static copy when the handler field must be wired at runtime.
static bb_err_t registry_add(const bb_route_t *route)
{
    if (s_count >= BB_ROUTE_REGISTRY_CAP) {
        bb_log_e(TAG, "route registry full (cap=%d); dropping descriptor for %s",
                 BB_ROUTE_REGISTRY_CAP, route->path ? route->path : "(null)");
        if (bb_http_route_registry_strict_would_fire()) {
#ifdef BB_HTTP_TESTING
            // Host-testable trap hook only -- see its definition above.
            // The non-test (production/ESP-IDF) path below does not
            // depend on this symbol; it only calls
            // bb_http_route_registry_strict_would_fire(), which stays
            // defined in every build.
            s_strict_trap_hit = true;
#else
            // Loud trap in a real (non-test) build -- a process-aborting
            // assert() is not something a Unity host test can observe as a
            // passing result, so a host test instead exercises the STRICT
            // decision directly via bb_http_route_registry_strict_would_fire()
            // and the fail-closed BB_ERR_NO_SPACE return below (same
            // BB_*_TESTING convention as bb_data_scratch_acquire()).
            assert(0 && "route registry overflow — increase BB_HTTP_ROUTE_REGISTRY_CAP");
#endif
        }
        return BB_ERR_NO_SPACE;
    }
    s_registry[s_count++] = route;

    // High-watermark warn: fire once when count crosses CAP-8, same
    // one-shot-latch idiom as bb_dispatch_api_add()'s s_dispatch_warned.
    if (!s_watermark_warned && s_count + 8 >= BB_ROUTE_REGISTRY_CAP) {
        s_watermark_warned = true;
        bb_log_w(TAG, "route registry high-watermark: %u/%u descriptors — consider raising BB_HTTP_ROUTE_REGISTRY_CAP",
                 (unsigned)s_count, (unsigned)BB_ROUTE_REGISTRY_CAP);
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Described route registration
// ---------------------------------------------------------------------------

// Equivalent to bb_http_register_described_route() with handler=NULL (schema-only
// descriptor registration).  Retained as a distinct entry point only for bb_ws_server,
// whose handler type (bb_ws_server_handler_fn) is incompatible with bb_route_t.handler.
// New schema-only routes should prefer bb_http_register_described_route() with a NULL
// handler.
bb_err_t bb_http_register_route_descriptor_only(const bb_route_t *route)
{
    if (!route) return BB_ERR_INVALID_ARG;
    return registry_add(route);
}

bb_err_t bb_http_register_described_route(bb_http_handle_t server,
                                          const bb_route_t *route)
{
    if (!route) return BB_ERR_INVALID_ARG;

    // route->handler == NULL means "schema-only": the request is served some
    // other way (e.g. a handler with an incompatible signature registered
    // directly, as bb_ws_server does) or not served at all. Skip the httpd/
    // dispatch wiring step entirely and just add the descriptor to the
    // registry — equivalent to bb_http_register_route_descriptor_only, but
    // reachable through the single unified entry point.
    if (!route->handler) {
        return registry_add(route);
    }

    // Delegate to the existing imperative registration first.
    bb_err_t err = bb_http_register_route(server, route->method, route->path, route->handler);
    if (err != BB_OK) {
        return err;
    }

    // Only add to the registry on success.
    return registry_add(route);
}

bb_err_t bb_http_register_route_table(bb_http_handle_t server,
                                      const bb_route_t * const *table,
                                      size_t n)
{
    if (!table) return BB_ERR_INVALID_ARG;
    for (size_t i = 0; i < n; i++) {
        bb_err_t err = bb_http_register_described_route(server, table[i]);
        if (err != BB_OK) return err;
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// URI registration check
// ---------------------------------------------------------------------------

// Paths for the two internal catch-all wildcards registered by bb_http itself.
// These are excluded from "is this URI registered?" because they match every
// URI — a bogus path under them must still yield 404, not 405.
#define BB_HTTP_PREFLIGHT_URI  "/*"   // OPTIONS /*  — CORS preflight
#define BB_HTTP_ASSET_URI      "/*"   // GET /*      — static asset wildcard

bool bb_http_uri_is_registered(const char *uri)
{
    if (!uri) return false;

    // Query-stripping stays local to this call site (bb_route_uri_match owns
    // only the pattern-vs-bounded-uri predicate, not query parsing) — compute
    // the match bound once, same convention as bb_dispatch_api_lookup.
    size_t path_len = 0;
    while (uri[path_len] != '\0' && uri[path_len] != '?') {
        path_len++;
    }

    for (size_t i = 0; i < s_count; i++) {
        const bb_route_t *r = s_registry[i];
        if (!r || !r->path) continue;  // LCOV_EXCL_BR_LINE — !r is unreachable; registry_add never stores NULL

        // Skip the two internal catch-all wildcard routes so that a path that
        // only matches via "/*" (preflight or asset wildcard) is not considered
        // "registered" for the purpose of 404 vs 405 disambiguation.
        if (strcmp(r->path, BB_HTTP_PREFLIGHT_URI) == 0 &&
            r->method == BB_HTTP_OPTIONS) continue;
        if (strcmp(r->path, BB_HTTP_ASSET_URI) == 0 &&
            r->method == BB_HTTP_GET) continue;

        if (bb_route_uri_match(r->path, uri, path_len)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// API dispatch table — (method,path) -> handler exact-match lookup for
// /api/* routes. Declarations live in bb_http_server.h (see BB_DISPATCH_API_CAP).
// ---------------------------------------------------------------------------

// Internal route entry
typedef struct {
    bb_http_method_t  method;
    const char       *path;
    bb_http_handler_fn handler;
    bool               is_wildcard;  // path's last byte is '*' at add time
    size_t             prefix_len;   // strlen(path)-1 when is_wildcard; unused otherwise
} bb_dispatch_api_entry_t;

// File-scope state — no heap, no ESP, s_ prefix per house rules
static bb_dispatch_api_entry_t s_dispatch[BB_DISPATCH_API_CAP];
static size_t                  s_dispatch_count;
static bool                    s_dispatch_warned;

void bb_dispatch_api_reset(void)
{
    memset(s_dispatch, 0, sizeof(s_dispatch));
    s_dispatch_count  = 0;
    s_dispatch_warned = false;
}

bb_err_t bb_dispatch_api_add(bb_http_method_t method, const char *path,
                              bb_http_handler_fn handler)
{
    if (s_dispatch_count >= BB_DISPATCH_API_CAP) {
        return BB_ERR_NO_SPACE;
    }

    // Duplicate-route safeguard: scan for an existing (method, path) pair.
    // Uses the same case-sensitive exact-match semantics as the dispatcher.
    if (path != NULL) {
        size_t path_len = strlen(path);
        for (size_t i = 0; i < s_dispatch_count; i++) {
            if (s_dispatch[i].method != method) continue;
            if (s_dispatch[i].path == NULL) continue;
            if (strlen(s_dispatch[i].path) != path_len) continue;
            if (memcmp(s_dispatch[i].path, path, path_len) == 0) {
                bb_log_w(TAG, "duplicate route %d %s ignored (first registration wins)",
                         (int)method, path);
#if defined(CONFIG_BB_HTTP_ROUTE_DUP_STRICT) && CONFIG_BB_HTTP_ROUTE_DUP_STRICT
                bb_log_e(TAG, "duplicate route %d %s — aborting (BB_HTTP_ROUTE_DUP_STRICT)",
                         (int)method, path);
                assert(0 && "duplicate (method,path) route registration — increase route uniqueness or disable BB_HTTP_ROUTE_DUP_STRICT");
#endif
                return BB_ERR_INVALID_STATE;
            }
        }
    }

    size_t plen = path ? strlen(path) : 0;
    bool   is_wildcard = plen > 0 && path[plen - 1] == '*';

    s_dispatch[s_dispatch_count].method      = method;
    s_dispatch[s_dispatch_count].path        = path;
    s_dispatch[s_dispatch_count].handler     = handler;
    s_dispatch[s_dispatch_count].is_wildcard = is_wildcard;
    s_dispatch[s_dispatch_count].prefix_len  = is_wildcard ? plen - 1 : 0;
    s_dispatch_count++;

    // High-watermark warn: fire once when count crosses CAP-8.
    if (!s_dispatch_warned && s_dispatch_count >= (size_t)(BB_DISPATCH_API_CAP - 8)) {
        s_dispatch_warned = true;
        bb_log_w(TAG, "api dispatch table at %u/%u; %d slots remain",
                 (unsigned)s_dispatch_count, (unsigned)BB_DISPATCH_API_CAP,
                 (int)(BB_DISPATCH_API_CAP - (int)s_dispatch_count));
    }

    return BB_OK;
}

// Two-pass, order-independent lookup:
//   Pass 1 (exact entries only): an exact path match always wins, regardless
//   of registration order relative to any wildcard entry. A path match with a
//   mismatched method is remembered — an exact route claims its path even
//   under a method mismatch, so a wildcard must never rescue it.
//   Pass 2 (wildcard entries, only run when pass 1 found no exact PATH
//   match): among wildcard entries whose prefix matches uri, the longest
//   matching prefix wins (most-specific route). Method match on that entry
//   is a HIT; mismatch is METHOD_MISMATCH; no wildcard prefix match is MISS.
bb_dispatch_api_result_t bb_dispatch_api_lookup(bb_http_method_t method,
                                                const char *uri,
                                                bb_http_handler_fn *out_handler)
{
    if (uri == NULL || out_handler == NULL) {
        return BB_DISPATCH_API_MISS;
    }

    // Compute path length: up to first '?' (strip query string).
    size_t path_len = 0;
    while (uri[path_len] != '\0' && uri[path_len] != '?') {
        path_len++;
    }

    // -------------------------------------------------------------------
    // Pass 1: exact entries only.
    // -------------------------------------------------------------------
    bool exact_path_matched = false;

    for (size_t i = 0; i < s_dispatch_count; i++) {
        if (s_dispatch[i].is_wildcard) {
            continue;
        }
        const char *entry_path = s_dispatch[i].path;
        if (entry_path == NULL) {
            continue;
        }

        // Exact match: same length and same bytes, entry NUL-terminated.
        // bb_route_uri_match's exact branch (entry has no trailing '*',
        // guaranteed by the is_wildcard skip above) is exactly this test.
        if (!bb_route_uri_match(entry_path, uri, path_len)) {
            continue;
        }

        // Path matches.
        exact_path_matched = true;

        if (s_dispatch[i].method == method) {
            *out_handler = s_dispatch[i].handler;
            return BB_DISPATCH_API_HIT;
        }
    }

    if (exact_path_matched) {
        // An exact route claims this path; a wildcard must not rescue it.
        return BB_DISPATCH_API_METHOD_MISMATCH;
    }

    // -------------------------------------------------------------------
    // Pass 2: wildcard entries — longest matching prefix wins.
    // -------------------------------------------------------------------
    size_t best_prefix_len = 0;
    bool   best_found      = false;
    bool   best_method_ok  = false;
    bb_http_handler_fn best_handler = NULL;

    for (size_t i = 0; i < s_dispatch_count; i++) {
        if (!s_dispatch[i].is_wildcard) {
            continue;
        }
        // entry_path is guaranteed non-NULL here: is_wildcard is only ever
        // set true when path is non-NULL and ends in '*' (see
        // bb_dispatch_api_add), so a NULL-path entry always has
        // is_wildcard == false and is filtered out by the check above.
        const char *entry_path = s_dispatch[i].path;
        if (!bb_route_uri_match(entry_path, uri, path_len)) {
            continue;
        }

        // Longest prefix wins; ties keep the first-found (registration order).
        if (!best_found || s_dispatch[i].prefix_len > best_prefix_len) {
            best_found      = true;
            best_prefix_len = s_dispatch[i].prefix_len;
            best_method_ok  = (s_dispatch[i].method == method);
            best_handler    = s_dispatch[i].handler;
        }
    }

    if (!best_found) {
        return BB_DISPATCH_API_MISS;
    }
    if (!best_method_ok) {
        return BB_DISPATCH_API_METHOD_MISMATCH;
    }

    *out_handler = best_handler;
    return BB_DISPATCH_API_HIT;
}

size_t bb_dispatch_api_count(void)
{
    return s_dispatch_count;
}

// ---------------------------------------------------------------------------
// Shared bb_http_req_uri() helper — strip query string, truncate-copy path
// ---------------------------------------------------------------------------

bb_err_t bb_http_uri_strip_query_copy(const char *uri, char *out, size_t out_cap)
{
    if (!uri || !out || out_cap == 0) return BB_ERR_INVALID_ARG;

    const char *q = strchr(uri, '?');
    size_t path_len = q ? (size_t)(q - uri) : strlen(uri);
    size_t copy_len = path_len < out_cap ? path_len : out_cap - 1;

    // bb_strlcpy copies from a NUL-terminated src; uri may have a query
    // string beyond path_len, so bound the copy via out_cap and NUL it at
    // copy_len ourselves rather than relying on bb_strlcpy's own strlen(src).
    bb_strlcpy(out, uri, copy_len + 1);
    return BB_OK;
}
