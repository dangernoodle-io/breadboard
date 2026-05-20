#include "bb_event_routes.h"
#include "bb_event.h"
#include "bb_event_ring.h"
#include "bb_log.h"
#include "bb_event_routes_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*bb_event_routes_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_event_routes_free_fn)(void *p);
static bb_event_routes_calloc_fn s_calloc = calloc;
static bb_event_routes_free_fn   s_free   = free;

#ifdef BB_EVENT_ROUTES_TESTING
void bb_event_routes_set_allocator(bb_event_routes_calloc_fn c, bb_event_routes_free_fn f) {
    s_calloc = c ? c : calloc;
    s_free   = f ? f : free;
}
void bb_event_routes_reset_allocator(void) {
    s_calloc = calloc;
    s_free   = free;
}
#endif

static const char *TAG = "bb_event_routes";

#ifndef CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS
#define CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS 4
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH
#define CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH 32
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_RING_CAPACITY
#define CONFIG_BB_EVENT_ROUTES_RING_CAPACITY 16
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY
#define CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY 256
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_HEARTBEAT_MS
#define CONFIG_BB_EVENT_ROUTES_HEARTBEAT_MS 15000
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_MAX_TOPICS
#define CONFIG_BB_EVENT_ROUTES_MAX_TOPICS 8
#endif

#define TOPIC_NAME_MAX 32

// ---------------------------------------------------------------------------
// Attached topics
// ---------------------------------------------------------------------------

typedef struct {
    char name[TOPIC_NAME_MAX];
    bb_event_topic_t topic;
    bb_event_ring_t ring;
} attached_topic_t;

static attached_topic_t s_topics[CONFIG_BB_EVENT_ROUTES_MAX_TOPICS];
static size_t s_num_topics = 0;

// ---------------------------------------------------------------------------
// Client slots
// ---------------------------------------------------------------------------

typedef struct queue_entry {
    int topic_idx;       // index into s_topics
    int32_t event_id;    // bb_event id
    size_t size;
    uint8_t *payload;    // borrowed; allocated alongside slot
} queue_entry_t;

struct bb_event_routes_client {
    atomic_bool in_use;

    // Ring queue
    queue_entry_t *entries;       // [queue_depth]
    uint8_t *payload_buf;         // [queue_depth * ring_max_entry]
    size_t queue_depth;
    size_t max_entry;
    size_t head;                  // write
    size_t tail;                  // read
    size_t count;
    uint64_t dropped;             // SSE id jumps reflect this
    uint64_t next_emit_id;        // monotonic id assigned at frame emit

    // One subscription per attached topic; entries[i] valid iff i<s_num_topics_snapshot.
    bb_event_sub_t subs[CONFIG_BB_EVENT_ROUTES_MAX_TOPICS];
    size_t num_subs;

    void *port_lock;              // platform-supplied mutex (opaque)
    void *event;                  // platform-supplied signal (opaque) — wakes SSE writer
};

static bb_event_routes_client_t s_clients[CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS];

// ---------------------------------------------------------------------------
// Config (resolved at init)
// ---------------------------------------------------------------------------

static struct {
    size_t max_clients;
    size_t queue_depth;
    size_t ring_capacity;
    size_t ring_max_entry;
    uint32_t heartbeat_ms;
    bool initialized;
} s_cfg;

// ---------------------------------------------------------------------------
// Capture: bb_event subscriber callback. Enqueues to a client slot.
// ---------------------------------------------------------------------------

static void capture_cb(bb_event_topic_t topic, int32_t id,
                       const void *data, size_t size, void *user)
{
    bb_event_routes_client_t *c = (bb_event_routes_client_t *)user;
    if (!c || !atomic_load(&c->in_use)) return;  // LCOV_EXCL_BR_LINE — defensive

    // Locate topic index. Linear scan over a small array is fine.
    int topic_idx = -1;
    for (size_t i = 0; i < s_num_topics; i++) {  // LCOV_EXCL_BR_LINE — capture only fires when a topic is attached
        if (s_topics[i].topic == topic) {
            topic_idx = (int)i;
            break;
        }
    }
    if (topic_idx < 0) return;  // LCOV_EXCL_LINE — attach guarantees a match

    size_t copy = size > c->max_entry ? c->max_entry : size;

    bb_event_routes_port_lock(c->port_lock);

    if (c->count == c->queue_depth) {
        // Drop oldest
        c->tail = (c->tail + 1) % c->queue_depth;
        c->count--;
        c->dropped++;
    }

    size_t w = c->head;
    queue_entry_t *e = &c->entries[w];
    e->topic_idx = topic_idx;
    e->event_id = id;
    e->size = copy;
    e->payload = c->payload_buf + (w * c->max_entry);
    if (copy > 0 && data) {  // LCOV_EXCL_BR_LINE — dispatch supplies valid data
        memcpy(e->payload, data, copy);
    }

    c->head = (c->head + 1) % c->queue_depth;
    c->count++;

    bb_event_routes_port_unlock(c->port_lock);

    bb_event_routes_port_event_signal(c->event);
}

// ---------------------------------------------------------------------------
// Public init / attach
// ---------------------------------------------------------------------------

bb_err_t bb_event_routes_init(const bb_event_routes_cfg_t *cfg)
{
    if (s_cfg.initialized) return BB_OK;

    s_cfg.max_clients    = (cfg && cfg->max_clients)        ? cfg->max_clients        : CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS;
    s_cfg.queue_depth    = (cfg && cfg->per_client_queue)   ? cfg->per_client_queue   : CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH;
    s_cfg.ring_capacity  = (cfg && cfg->ring_capacity)      ? cfg->ring_capacity      : CONFIG_BB_EVENT_ROUTES_RING_CAPACITY;
    s_cfg.ring_max_entry = (cfg && cfg->ring_max_entry)     ? cfg->ring_max_entry     : CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY;
    s_cfg.heartbeat_ms   = (cfg && cfg->heartbeat_ms)       ? cfg->heartbeat_ms       : CONFIG_BB_EVENT_ROUTES_HEARTBEAT_MS;

    if (s_cfg.max_clients > CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS) {
        return BB_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS; i++) {
        atomic_store(&s_clients[i].in_use, false);
    }

    s_cfg.initialized = true;
    bb_log_i(TAG, "initialized: max_clients=%zu queue_depth=%zu ring=%zu/%zu hb=%ums",
             s_cfg.max_clients, s_cfg.queue_depth, s_cfg.ring_capacity,
             s_cfg.ring_max_entry, (unsigned)s_cfg.heartbeat_ms);
    return BB_OK;
}

bb_err_t bb_event_routes_attach_ex(const char *topic_name, bool retained)
{
    if (!topic_name) return BB_ERR_INVALID_ARG;
    if (!s_cfg.initialized) return BB_ERR_INVALID_STATE;

    // Dedupe
    for (size_t i = 0; i < s_num_topics; i++) {
        if (strcmp(s_topics[i].name, topic_name) == 0) return BB_OK;
    }

    if (s_num_topics >= CONFIG_BB_EVENT_ROUTES_MAX_TOPICS) {
        bb_log_e(TAG, "topic table full");
        return BB_ERR_NO_SPACE;
    }

    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_lookup(topic_name, &topic);
    if (err != BB_OK) {
        bb_log_e(TAG, "topic '%s' not registered", topic_name);
        return err;
    }

    bb_event_ring_t ring = NULL;
    err = bb_event_ring_attach_ex(topic, s_cfg.ring_capacity, s_cfg.ring_max_entry,
                                  retained, &ring);
    if (err != BB_OK) {
        bb_log_e(TAG, "ring attach failed for '%s': %d", topic_name, err);
        return err;
    }

    attached_topic_t *t = &s_topics[s_num_topics++];
    strncpy(t->name, topic_name, TOPIC_NAME_MAX - 1);
    t->name[TOPIC_NAME_MAX - 1] = '\0';
    t->topic = topic;
    t->ring = ring;
    bb_log_d(TAG, "attached '%s' retained=%d", topic_name, (int)retained);
    return BB_OK;
}

bb_err_t bb_event_routes_attach(const char *topic_name)
{
    return bb_event_routes_attach_ex(topic_name, false);
}

// ---------------------------------------------------------------------------
// Client lifecycle (called by route handler)
// ---------------------------------------------------------------------------

bb_err_t bb_event_routes_client_acquire_ex(bb_event_routes_client_t **out,
                                           const char *topic_filter)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (!s_cfg.initialized) return BB_ERR_INVALID_STATE;

    for (size_t i = 0; i < s_cfg.max_clients; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&s_clients[i].in_use, &expected, true)) {
            bb_event_routes_client_t *c = &s_clients[i];
            c->queue_depth = s_cfg.queue_depth;
            c->max_entry = s_cfg.ring_max_entry;
            c->head = c->tail = c->count = 0;
            c->dropped = 0;
            c->next_emit_id = 1;
            c->num_subs = 0;
            c->entries = (queue_entry_t *)s_calloc(c->queue_depth, sizeof(queue_entry_t));
            c->payload_buf = (uint8_t *)s_calloc(c->queue_depth, c->max_entry);
            c->port_lock = bb_event_routes_port_lock_create();
            c->event = bb_event_routes_port_event_create();
            if (!c->entries || !c->payload_buf || !c->port_lock || !c->event) {  // LCOV_EXCL_BR_LINE — port_lock alloc covered by entries/payload paths
                s_free(c->entries);
                s_free(c->payload_buf);
                if (c->port_lock) bb_event_routes_port_lock_destroy(c->port_lock);  // LCOV_EXCL_BR_LINE
                if (c->event) bb_event_routes_port_event_destroy(c->event);  // LCOV_EXCL_BR_LINE — paired with port_lock alloc-failure path
                c->entries = NULL;
                c->payload_buf = NULL;
                c->port_lock = NULL;
                c->event = NULL;
                atomic_store(&c->in_use, false);
                return BB_ERR_NO_SPACE;
            }

            // Subscribe to matching topics. Replay-on-connect via ring.
            for (size_t k = 0; k < s_num_topics; k++) {
                // Filter to topic_filter if specified.
                if (topic_filter && strcmp(s_topics[k].name, topic_filter) != 0) {
                    continue;
                }
                bb_event_sub_t sub = NULL;
                bb_err_t err = bb_event_ring_subscribe_with_replay(
                    s_topics[k].ring, capture_cb, c, &sub);
                if (err != BB_OK) {
                    // Roll back: unsubscribe what we already did, release.
                    for (size_t j = 0; j < c->num_subs; j++) {
                        bb_event_unsubscribe(c->subs[j]);
                    }
                    s_free(c->entries);
                    s_free(c->payload_buf);
                    bb_event_routes_port_lock_destroy(c->port_lock);
                    bb_event_routes_port_event_destroy(c->event);
                    c->entries = NULL;
                    c->payload_buf = NULL;
                    c->port_lock = NULL;
                    c->event = NULL;
                    atomic_store(&c->in_use, false);
                    return err;
                }
                c->subs[c->num_subs++] = sub;
            }

            *out = c;
            return BB_OK;
        }
    }
    return BB_ERR_NO_SPACE;
}

bb_err_t bb_event_routes_client_acquire(bb_event_routes_client_t **out)
{
    return bb_event_routes_client_acquire_ex(out, NULL);
}

void bb_event_routes_client_release(bb_event_routes_client_t *c)
{
    if (!c) return;
    for (size_t i = 0; i < c->num_subs; i++) {
        bb_event_unsubscribe(c->subs[i]);
    }
    c->num_subs = 0;
    s_free(c->entries);
    s_free(c->payload_buf);
    if (c->port_lock) bb_event_routes_port_lock_destroy(c->port_lock);  // LCOV_EXCL_BR_LINE — port_lock always set on release path
    if (c->event) bb_event_routes_port_event_destroy(c->event);  // LCOV_EXCL_BR_LINE — event always set on release path
    c->entries = NULL;
    c->payload_buf = NULL;
    c->port_lock = NULL;
    c->event = NULL;
    atomic_store(&c->in_use, false);
}

// ---------------------------------------------------------------------------
// Frame builder (drains one entry; caller handles transport)
// ---------------------------------------------------------------------------

size_t bb_event_routes_drain_frame(bb_event_routes_client_t *c, char *buf, size_t buflen)
{
    if (!c || !buf || buflen < 32) return 0;

    bb_event_routes_port_lock(c->port_lock);
    if (c->count == 0) {
        bb_event_routes_port_unlock(c->port_lock);
        return 0;
    }
    queue_entry_t e = c->entries[c->tail];
    // Copy payload locally so we can release the lock before serializing.
    uint8_t local_payload[CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY];
    // e.size is already capped at c->max_entry <= ring_max_entry; cap defensively.
    size_t copy = e.size > sizeof(local_payload) ? sizeof(local_payload) : e.size;  // LCOV_EXCL_BR_LINE
    if (copy > 0) memcpy(local_payload, e.payload, copy);
    c->tail = (c->tail + 1) % c->queue_depth;
    c->count--;
    uint64_t emit_id = c->next_emit_id + c->dropped;
    c->next_emit_id = emit_id + 1;
    uint64_t carried_drops = c->dropped;
    c->dropped = 0;
    bb_event_routes_port_unlock(c->port_lock);

    const char *topic_name = s_topics[e.topic_idx].name;
    int n;
    if (copy == 0) {
        n = snprintf(buf, buflen, "event: %s\ndata: {}\nid: %llu\n\n",
                     topic_name, (unsigned long long)emit_id);
    } else {
        n = snprintf(buf, buflen, "event: %s\ndata: %.*s\nid: %llu\n\n",
                     topic_name, (int)copy, (const char *)local_payload,
                     (unsigned long long)emit_id);
    }
    if (n < 0 || (size_t)n >= buflen) {  // LCOV_EXCL_BR_LINE — n<0 unreachable for printf format
        // Truncated; emit a safe fallback so the SSE frame is still valid.
        n = snprintf(buf, buflen, "event: %s\ndata: {\"truncated\":true}\nid: %llu\n\n",
                     topic_name, (unsigned long long)emit_id);
    }
    (void)carried_drops;  // visible via /api/events status endpoint in a future iteration
    return (size_t)(n > 0 ? n : 0);  // LCOV_EXCL_BR_LINE — fallback frame always positive
}

uint32_t bb_event_routes_heartbeat_ms(void) { return s_cfg.heartbeat_ms; }

void *bb_event_routes_client_event(bb_event_routes_client_t *c)
{
    return c ? c->event : NULL;
}

// ---------------------------------------------------------------------------
// Diagnostics accessors (public, no ESP-IDF deps)
// ---------------------------------------------------------------------------

size_t bb_event_routes_topic_count(void)
{
    return s_num_topics;
}

bb_err_t bb_event_routes_topic_info(size_t idx,
                                    const char **name,
                                    bb_event_ring_t *ring)
{
    if (idx >= s_num_topics) return BB_ERR_NOT_FOUND;
    if (name) *name = s_topics[idx].name;
    if (ring) *ring = s_topics[idx].ring;
    return BB_OK;
}

size_t bb_event_routes_active_client_count(void)
{
    size_t active = 0;
    for (size_t i = 0; i < CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS; i++) {
        if (atomic_load(&s_clients[i].in_use)) active++;
    }
    return active;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_EVENT_ROUTES_TESTING
void bb_event_routes_reset_for_test(void) {
    for (size_t i = 0; i < CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS; i++) {
        if (atomic_load(&s_clients[i].in_use)) {
            bb_event_routes_client_release(&s_clients[i]);
        }
    }
    for (size_t i = 0; i < s_num_topics; i++) {
        if (s_topics[i].ring) bb_event_ring_detach(s_topics[i].ring);  // LCOV_EXCL_BR_LINE
    }
    s_num_topics = 0;
    memset(&s_cfg, 0, sizeof(s_cfg));
}

size_t bb_event_routes_queued_for_test(bb_event_routes_client_t *c) {
    return c ? c->count : 0;  // LCOV_EXCL_BR_LINE
}

uint64_t bb_event_routes_dropped_for_test(bb_event_routes_client_t *c) {
    return c ? c->dropped : 0;  // LCOV_EXCL_BR_LINE
}
#endif
