// bb_event_topic_registry — host/ESP-IDF backend wrapping the generic
// bb_registry primitive. Compiled alongside bb_event_routes_common.c on host
// and ESP-IDF (as part of the bb_event_routes component). Arduino uses its
// own pthread-free linear-array implementation in
// platform/arduino/bb_event_routes/bb_event_routes_arduino.cpp instead —
// see bb_event_topic_registry.h for why the two are split.

#include "bb_event_topic_registry.h"
#include "bb_registry.h"
#include "bb_log.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* Kconfig bridge: honour CONFIG_BB_EVENT_ROUTES_MAX_TOPICS from build flags;
 * default 8 (matches Kconfig default and bb_event_routes_common.c). */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_MAX_TOPICS
#define CONFIG_BB_EVENT_ROUTES_MAX_TOPICS 8
#endif

static const char *TAG = "bb_event_topic_registry";

BB_REGISTRY_DEFINE_TAGGED(s_topic_registry, CONFIG_BB_EVENT_ROUTES_MAX_TOPICS, "event_topics");

// Wrapper mutex — mirrors bb_ring_registry's s_ring_reg_lock: serialises the
// public ops as a single atomic unit over the top of the primitive's own
// internal lock. There is no deregister here (register-only), so none of
// bb_ring_registry's TOCTOU/UAF concerns apply directly, but the wrapper
// keeps this component's locking shape consistent with its sibling and
// gives it its own lock to extend should a deregister path ever be added.
static pthread_mutex_t s_topic_reg_lock = PTHREAD_MUTEX_INITIALIZER;

bb_err_t bb_event_topic_registry_register(const char *name, bb_event_attached_topic_t *t)
{
    if (!name || !t) {
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&s_topic_reg_lock);
    bb_err_t err = bb_registry_register(&s_topic_registry, name, (void *)t);
    pthread_mutex_unlock(&s_topic_reg_lock);

    if (err == BB_ERR_INVALID_STATE) {
        // Duplicate name — bb_event_routes_attach* is idempotent per topic.
        return BB_OK;
    }
    if (err != BB_OK) {
        bb_log_w(TAG, "register('%s') failed: %d", name, (int)err);
    }
    return err;
}

// find_by_handle scan state.
typedef struct {
    bb_event_topic_t target;
    bool found;
    size_t match_idx;
    size_t cursor;
} find_by_handle_t;

static void find_by_handle_cb(const char *name, void *value, void *ctx)
{
    (void)name;
    find_by_handle_t *scan = (find_by_handle_t *)ctx;
    const bb_event_attached_topic_t *t = (const bb_event_attached_topic_t *)value;
    if (!scan->found && t->topic == scan->target) {
        scan->found = true;
        scan->match_idx = scan->cursor;
    }
    scan->cursor++;
}

bb_err_t bb_event_topic_registry_find_by_handle(bb_event_topic_t topic, size_t *out_idx)
{
    if (!topic || !out_idx) {
        return BB_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&s_topic_reg_lock);
    find_by_handle_t scan = { .target = topic, .found = false, .match_idx = 0, .cursor = 0 };
    bb_registry_foreach(&s_topic_registry, find_by_handle_cb, &scan);
    pthread_mutex_unlock(&s_topic_reg_lock);

    if (!scan.found) {
        return BB_ERR_NOT_FOUND;
    }
    *out_idx = scan.match_idx;
    return BB_OK;
}

size_t bb_event_topic_registry_count(void)
{
    pthread_mutex_lock(&s_topic_reg_lock);
    uint16_t count = bb_registry_count(&s_topic_registry);
    pthread_mutex_unlock(&s_topic_reg_lock);
    return (size_t)count;
}

bb_err_t bb_event_topic_registry_get_by_index(size_t idx, bb_event_attached_topic_t **out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }
    if (idx > UINT16_MAX) {
        return BB_ERR_NOT_FOUND;
    }

    pthread_mutex_lock(&s_topic_reg_lock);
    bb_registry_entry_t entry;
    bb_err_t err = bb_registry_get_by_index(&s_topic_registry, (uint16_t)idx, &entry);
    pthread_mutex_unlock(&s_topic_reg_lock);

    if (err != BB_OK) {
        return err;
    }
    *out = (bb_event_attached_topic_t *)entry.value;
    return BB_OK;
}

#ifdef BB_EVENT_ROUTES_TESTING
void bb_event_topic_registry_test_reset(void)
{
    pthread_mutex_lock(&s_topic_reg_lock);
    bb_registry_reset(&s_topic_registry);
    pthread_mutex_unlock(&s_topic_reg_lock);
}
#endif
