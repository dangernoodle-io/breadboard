#include "bb_http.h"
#include "bb_log.h"
#include <stddef.h>
#include <stdint.h>

// Maximum number of described routes the registry holds.
// Overflow is logged and the descriptor is silently dropped.
#define BB_ROUTE_REGISTRY_CAP 64

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

static bb_err_t registry_add(const bb_route_t *route)
{
    if (s_count >= BB_ROUTE_REGISTRY_CAP) {
        bb_log_e(TAG, "route registry full (cap=%d); dropping descriptor for %s",
                 BB_ROUTE_REGISTRY_CAP, route->path ? route->path : "(null)");
        return BB_OK;  // overflow is non-fatal
    }
    s_registry[s_count++] = route;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Described route registration
// ---------------------------------------------------------------------------

bb_err_t bb_http_register_route_descriptor_only(const bb_route_t *route)
{
    if (!route) return BB_ERR_INVALID_ARG;
    return registry_add(route);
}

bb_err_t bb_http_register_described_route(bb_http_handle_t server,
                                          const bb_route_t *route)
{
    if (!route) return BB_ERR_INVALID_ARG;

    // Delegate to the existing imperative registration first.
    bb_err_t err = bb_http_register_route(server, route->method, route->path, route->handler);
    if (err != BB_OK) {
        return err;
    }

    // Only add to the registry on success.
    return registry_add(route);
}
