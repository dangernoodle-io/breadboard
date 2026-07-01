#include "bb_http.h"
#include "bb_log.h"
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

static const char *TAG = "bb_http_registry";

// Process-wide static registry.  Stores pointers only — descriptor
// lifetime is the caller's responsibility (same convention as bb_http_asset_t).
static const bb_route_t *s_registry[BB_ROUTE_REGISTRY_CAP];
static size_t             s_count = 0;

// ---------------------------------------------------------------------------
// Public registry API
// ---------------------------------------------------------------------------

void bb_http_route_registry_clear(void)
{
    for (size_t i = 0; i < s_count; i++) {
        s_registry[i] = NULL;
    }
    s_count = 0;
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
        return BB_ERR_NO_SPACE;
    }
    s_registry[s_count++] = route;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Described route registration
// ---------------------------------------------------------------------------

// Equivalent to bb_http_register_described_route() with handler=NULL (schema-only
// descriptor registration).  Retained as a distinct entry point only for bb_websocket,
// whose handler type (bb_websocket_handler_fn) is incompatible with bb_route_t.handler.
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
    // directly, as bb_websocket does) or not served at all. Skip the httpd/
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

// Match a URI against a route path pattern using the same wildcard semantics
// as httpd_uri_match_wildcard: a pattern ending in '*' is a prefix match
// (the '*' acts as a suffix wildcard); anything else is an exact match.
// Strip query string from uri before comparing.
static bool uri_pattern_match(const char *pattern, const char *uri)
{
    if (!pattern || !uri) return false;  // LCOV_EXCL_BR_LINE — caller guards both before calling

    // Strip query string from uri for matching
    char path_buf[256];
    const char *q = strchr(uri, '?');
    if (q) {
        size_t plen = (size_t)(q - uri);
        if (plen >= sizeof(path_buf)) plen = sizeof(path_buf) - 1;
        memcpy(path_buf, uri, plen);
        path_buf[plen] = '\0';
        uri = path_buf;
    }

    size_t plen = strlen(pattern);
    if (plen > 0 && pattern[plen - 1] == '*') {
        // Wildcard: uri must start with everything before the '*'
        return strncmp(uri, pattern, plen - 1) == 0;
    }
    return strcmp(uri, pattern) == 0;
}

// Paths for the two internal catch-all wildcards registered by bb_http itself.
// These are excluded from "is this URI registered?" because they match every
// URI — a bogus path under them must still yield 404, not 405.
#define BB_HTTP_PREFLIGHT_URI  "/*"   // OPTIONS /*  — CORS preflight
#define BB_HTTP_ASSET_URI      "/*"   // GET /*      — static asset wildcard

bool bb_http_uri_is_registered(const char *uri)
{
    if (!uri) return false;

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

        if (uri_pattern_match(r->path, uri)) return true;
    }
    return false;
}
