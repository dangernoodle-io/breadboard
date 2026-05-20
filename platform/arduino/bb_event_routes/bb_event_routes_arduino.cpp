// Arduino stub for bb_event_routes — returns 503 not_implemented.
// The full SSE machinery (per-client tasks, ring queues, dispatch fan-out) is
// too heavy for current Arduino consumers (CC3000 on AVR Uno). File a follow-up
// when a real Arduino target with RAM headroom needs /api/events.
#include "bb_event_routes.h"
#include "bb_event_routes_internal.h"
#include "bb_log.h"

#include <stdbool.h>
#include <stdlib.h>

static const char *TAG = "bb_event_routes";

// Port shims: trivial no-op locks. The Arduino stub never spawns SSE clients,
// but bb_event_routes_init still resolves these symbols.
void *bb_event_routes_port_lock_create(void)
{
    static bool s_warned = false;
    if (!s_warned) {
        bb_log_w(TAG, "SSE (/api/events) is not supported on Arduino; clients receive 503");
        s_warned = true;
    }
    return (void *)1;
}
void  bb_event_routes_port_lock_destroy(void *lock)     { (void)lock; }
void  bb_event_routes_port_lock(void *lock)             { (void)lock; }
void  bb_event_routes_port_unlock(void *lock)           { (void)lock; }
void  bb_event_routes_port_notify(void *lock)           { (void)lock; }
