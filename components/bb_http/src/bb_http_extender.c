#include "bb_http_extender.h"
#include "bb_http_extender_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct {
    bb_http_extender_fn fn;
    const char         *schema_props; // NULL = no schema contribution
} extender_entry_t;

typedef struct {
    const char       *route_id;       // static string key
    extender_entry_t  entries[BB_HTTP_EXTENDER_MAX_PER_ROUTE];
    int               count;
    char             *assembled_schema; // malloc'd; NULL until first assemble call
} route_slot_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static route_slot_t s_routes[BB_HTTP_EXTENDER_MAX_ROUTES];
static int          s_route_count = 0;
static bool         s_frozen      = false;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Find slot for route_id (strcmp); returns pointer or NULL if not found.
static route_slot_t *find_route(const char *route_id)
{
    for (int i = 0; i < s_route_count; i++) {
        if (strcmp(s_routes[i].route_id, route_id) == 0) {
            return &s_routes[i];
        }
    }
    return NULL;
}

// Find or create a slot for route_id. Returns NULL if table full.
static route_slot_t *get_or_create_route(const char *route_id)
{
    route_slot_t *slot = find_route(route_id);
    if (slot) return slot;
    if (s_route_count >= BB_HTTP_EXTENDER_MAX_ROUTES) return NULL;
    slot = &s_routes[s_route_count++];
    slot->route_id        = route_id;
    slot->count           = 0;
    slot->assembled_schema = NULL;
    return slot;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_http_register_route_extender(const char *route_id,
                                          bb_http_extender_fn fn,
                                          const char *schema_props_fragment)
{
    if (!fn) return BB_ERR_INVALID_ARG;
    if (s_frozen) return BB_ERR_INVALID_STATE;

    route_slot_t *slot = get_or_create_route(route_id);
    if (!slot) return BB_ERR_NO_SPACE;
    if (slot->count >= BB_HTTP_EXTENDER_MAX_PER_ROUTE) return BB_ERR_NO_SPACE;

    const char *frag = (schema_props_fragment && schema_props_fragment[0])
                       ? schema_props_fragment : NULL;
    slot->entries[slot->count].fn          = fn;
    slot->entries[slot->count].schema_props = frag;
    slot->count++;
    return BB_OK;
}

#ifdef ESP_PLATFORM
void bb_http_route_run_extenders(const char *route_id, bb_json_t root)
#else
void bb_http_route_run_extenders(const char *route_id, void *root)
#endif
{
    route_slot_t *slot = find_route(route_id);
    if (!slot) return;
    for (int i = 0; i < slot->count; i++) {
        slot->entries[i].fn(root);
    }
}

const char *bb_http_route_assemble_schema(const char *route_id,
                                           const char *base,
                                           const char *suffix)
{
    route_slot_t *slot = find_route(route_id);
    // If no slot, there are no extender fragments — just join base + suffix.
    // We still need to malloc+store so the caller gets a stable pointer.

    // Compute required length: base + ("," + frag)* + suffix + NUL
    size_t len = strlen(base) + strlen(suffix) + 1;
    if (slot) {
        for (int i = 0; i < slot->count; i++) {
            if (slot->entries[i].schema_props) {
                len += 1 + strlen(slot->entries[i].schema_props); // comma + frag
            }
        }
    }

    char *buf = malloc(len);
    if (!buf) return NULL;

    char *p = buf;
    p = stpcpy(p, base);
    if (slot) {
        for (int i = 0; i < slot->count; i++) {
            if (slot->entries[i].schema_props) {
                *p++ = ',';
                p = stpcpy(p, slot->entries[i].schema_props);
            }
        }
    }
    stpcpy(p, suffix);

    // Store for later retrieval (test hooks + re-use). If a previous buffer
    // exists (shouldn't happen in normal use), free it.
    if (slot) {
        free(slot->assembled_schema);
        slot->assembled_schema = buf;
    } else {
        // No slot means no extenders were registered. We need a place to store
        // the buffer. Create a slot now (freeze may already be true, which is fine
        // since we're not adding an extender — just a storage slot).
        // We temporarily bypass the freeze check for the internal slot creation.
        if (s_route_count < BB_HTTP_EXTENDER_MAX_ROUTES) {
            route_slot_t *new_slot = &s_routes[s_route_count++];
            new_slot->route_id         = route_id;
            new_slot->count            = 0;
            new_slot->assembled_schema = buf;
        }
        // If table is full we cannot store; buf is still valid until reset.
        // The caller (bb_info_init) immediately publishes into s_info_responses,
        // so the pointer lifetime is fine even without storing here.
    }

    return buf;
}

void bb_http_extender_freeze(void)
{
    s_frozen = true;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_HTTP_TESTING

void bb_http_extender_reset_for_test(void)
{
    for (int i = 0; i < s_route_count; i++) {
        free(s_routes[i].assembled_schema);
    }
    memset(s_routes, 0, sizeof(s_routes));
    s_route_count = 0;
    s_frozen      = false;
}

void bb_http_extender_freeze_for_test(void)
{
    s_frozen = true;
}

const char *bb_http_extender_get_assembled_schema(const char *route_id)
{
    route_slot_t *slot = find_route(route_id);
    return slot ? slot->assembled_schema : NULL;
}

#endif /* BB_HTTP_TESTING */
