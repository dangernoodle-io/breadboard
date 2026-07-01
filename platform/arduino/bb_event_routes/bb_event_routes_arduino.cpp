// Arduino stub for bb_event_routes — returns 503 not_implemented.
// The full SSE machinery (per-client tasks, ring queues, dispatch fan-out) is
// too heavy for current Arduino consumers (CC3000 on AVR Uno). File a follow-up
// when a real Arduino target with RAM headroom needs /api/events.
#include "bb_event_routes.h"
#include "bb_event_routes_internal.h"
#include "bb_event_topic_registry.h"
#include "bb_log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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

// ---------------------------------------------------------------------------
// bb_event_topic_registry — Arduino backend.
//
// Plain linear-array scan, no lock: Arduino's loop() is single-threaded, so
// none of the concurrency machinery in the host/ESP-IDF wrapper (which wraps
// the generic bb_registry primitive — a pthread-only dependency) is needed
// or available here. Same register-only API surface as that wrapper.
// ---------------------------------------------------------------------------

#ifndef CONFIG_BB_EVENT_ROUTES_MAX_TOPICS
#define CONFIG_BB_EVENT_ROUTES_MAX_TOPICS 8
#endif

static bb_event_attached_topic_t *s_topic_slots[CONFIG_BB_EVENT_ROUTES_MAX_TOPICS];
static size_t s_topic_count = 0;

bb_err_t bb_event_topic_registry_register(const char *name, bb_event_attached_topic_t *t)
{
    if (!name || !t) return BB_ERR_INVALID_ARG;
    for (size_t i = 0; i < s_topic_count; i++) {
        if (strcmp(s_topic_slots[i]->name, name) == 0) return BB_OK;  // duplicate — idempotent
    }
    if (s_topic_count >= CONFIG_BB_EVENT_ROUTES_MAX_TOPICS) return BB_ERR_NO_SPACE;
    s_topic_slots[s_topic_count++] = t;
    return BB_OK;
}

bb_err_t bb_event_topic_registry_find_by_handle(bb_event_topic_t topic, size_t *out_idx)
{
    if (!topic || !out_idx) return BB_ERR_INVALID_ARG;
    for (size_t i = 0; i < s_topic_count; i++) {
        if (s_topic_slots[i]->topic == topic) {
            *out_idx = i;
            return BB_OK;
        }
    }
    return BB_ERR_NOT_FOUND;
}

size_t bb_event_topic_registry_count(void) { return s_topic_count; }

bb_err_t bb_event_topic_registry_get_by_index(size_t idx, bb_event_attached_topic_t **out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (idx >= s_topic_count) return BB_ERR_NOT_FOUND;
    *out = s_topic_slots[idx];
    return BB_OK;
}

#ifdef BB_EVENT_ROUTES_TESTING
void bb_event_topic_registry_test_reset(void) { s_topic_count = 0; }
#endif
