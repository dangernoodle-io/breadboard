// Arduino stub for bb_event_routes — returns 503 not_implemented.
// The full SSE machinery (per-client tasks, ring queues, dispatch fan-out) is
// too heavy for current Arduino consumers (CC3000 on AVR Uno). File a follow-up
// when a real Arduino target with RAM headroom needs /api/events.
#include "bb_event_routes.h"
#include "bb_event_routes_internal.h"

#include <stdlib.h>

// Port shims: trivial no-op locks. The Arduino stub never spawns SSE clients,
// but bb_event_routes_init still resolves these symbols.
void *bb_event_routes_port_lock_create(void)            { return (void *)1; }
void  bb_event_routes_port_lock_destroy(void *lock)     { (void)lock; }
void  bb_event_routes_port_lock(void *lock)             { (void)lock; }
void  bb_event_routes_port_unlock(void *lock)           { (void)lock; }
void  bb_event_routes_port_notify(void *lock)           { (void)lock; }
