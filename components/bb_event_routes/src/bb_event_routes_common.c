#include "bb_event_routes.h"
#include "bb_event.h"
#include "bb_event_ring.h"
#include "bb_log.h"
#include "bb_event_routes_internal.h"
#include "bb_event_topic_registry.h"
#include "bb_event_routes_defaults.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*bb_event_routes_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_event_routes_free_fn)(void *p);
static bb_event_routes_calloc_fn s_calloc = calloc;
static bb_event_routes_free_fn   s_free   = free;

void bb_event_routes_set_allocator(bb_event_routes_calloc_fn c, bb_event_routes_free_fn f) {
    s_calloc = c ? c : calloc;
    s_free   = f ? f : free;
}
void bb_event_routes_reset_allocator(void) {
    s_calloc = calloc;
    s_free   = free;
}

static const char *TAG = "bb_event_routes";

#ifndef CONFIG_BB_EVENT_ROUTES_RING_CAPACITY
#define CONFIG_BB_EVENT_ROUTES_RING_CAPACITY 16
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_RETAINED_RING_CAPACITY
#define CONFIG_BB_EVENT_ROUTES_RETAINED_RING_CAPACITY 1
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

#define TOPIC_NAME_MAX BB_EVENT_TOPIC_NAME_MAX

// ---------------------------------------------------------------------------
// Attached topics
//
// Storage is a fixed array, indexed identically to bb_event_topic_registry's
// registration order (register-only, no deregister — see
// bb_event_topic_registry.h). The registry (a thin consumer of the generic
// bb_registry primitive on host/ESP-IDF; a plain array scan on Arduino)
// replaces the old manual dedupe-scan + lookup-by-handle bookkeeping.
// ---------------------------------------------------------------------------

typedef bb_event_attached_topic_t attached_topic_t;

static attached_topic_t s_topics[CONFIG_BB_EVENT_ROUTES_MAX_TOPICS];
static void *s_topics_lock = NULL;  // guards the compound attach_ex2 sequence below

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
// Optional static pool (CONFIG_BB_EVENT_ROUTES_STATIC_POOL)
// Pre-allocates per-client payload buffers and queue entry arrays at compile
// time as BSS arrays, reused across connect/disconnect cycles instead of
// malloc/free on each connection — zero fragmentation on no-PSRAM boards.
// ---------------------------------------------------------------------------

#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL || defined(BB_EVENT_ROUTES_TESTING)
static uint8_t s_payload_pool[CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS][CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH * CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY];
static queue_entry_t s_entry_pool[CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS][CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH];
#endif

#ifdef BB_EVENT_ROUTES_TESTING
// Runtime flag that lets the host test suite exercise the static-pool path
// without requiring a compile-time Kconfig change. Allows both dynamic and
// static paths to be tested in the same test binary.
static bool s_use_static_pool_for_test = false;
void bb_event_routes_set_static_pool_for_test(bool use_static) {
    s_use_static_pool_for_test = use_static;
}
#endif

// Returns true when the static-pool path is active (either via Kconfig or
// the test override).
static bool use_static_pool(void) {
#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL
    return true;
#elif defined(BB_EVENT_ROUTES_TESTING)
    return s_use_static_pool_for_test;
#else
    return false;
#endif
}

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

    // Locate topic index via the registry. bb_event_topic_registry_register
    // is only ever called (in attach_ex2) after the entry's topic/ring/name
    // fields are fully populated, and the registry's own internal lock
    // (host/ESP-IDF) establishes happens-before for that write — no need to
    // also hold s_topics_lock here.
    size_t topic_idx = 0;
    if (bb_event_topic_registry_find_by_handle(topic, &topic_idx) != BB_OK) {  // LCOV_EXCL_BR_LINE — attach guarantees a match
        return;  // LCOV_EXCL_LINE
    }

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
    e->topic_idx = (int)topic_idx;
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

    s_topics_lock = bb_event_routes_port_lock_create();
    if (!s_topics_lock) return BB_ERR_NO_SPACE;  // LCOV_EXCL_LINE — OOM on lock alloc

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

bb_err_t bb_event_routes_attach_ex2(const char *topic_name, bool retained,
                                    size_t max_entry)
{
    if (!topic_name) return BB_ERR_INVALID_ARG;
    if (!s_cfg.initialized) return BB_ERR_INVALID_STATE;

    // Hold the topics lock for the full attach: dedupe + bounds check + ring
    // ops + commit.  bb_event_ring_attach_ex uses its own bb_event_lock (a
    // separate lock from s_topics_lock) so there is no deadlock risk.
    // Holding the lock across the whole operation prevents TOCTOU without
    // requiring a re-check after re-acquire. (The registry also serialises
    // its own ops internally on host/ESP-IDF, but that alone would not be
    // enough here: the compound reserve-slot + ring-create + populate +
    // commit sequence below must run as a single atomic unit, which only
    // s_topics_lock — held across the whole function — guarantees.)
    bb_event_routes_port_lock(s_topics_lock);

    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_lookup(topic_name, &topic);
    if (err != BB_OK) {
        bb_event_routes_port_unlock(s_topics_lock);
        bb_log_e(TAG, "topic '%s' not registered", topic_name);
        return err;
    }

    size_t existing_idx;
    if (bb_event_topic_registry_find_by_handle(topic, &existing_idx) == BB_OK) {
        bb_event_routes_port_unlock(s_topics_lock);
        return BB_OK;  // already attached — idempotent
    }

    if (bb_event_topic_registry_count() >= CONFIG_BB_EVENT_ROUTES_MAX_TOPICS) {
        bb_event_routes_port_unlock(s_topics_lock);
        bb_log_e(TAG, "topic table full");
        return BB_ERR_NO_SPACE;
    }

    size_t ring_cap = retained
        ? CONFIG_BB_EVENT_ROUTES_RETAINED_RING_CAPACITY
        : s_cfg.ring_capacity;
    size_t entry_size = max_entry ? max_entry : s_cfg.ring_max_entry;
    bb_event_ring_t ring = NULL;
    err = bb_event_ring_attach_ex(topic, ring_cap, entry_size, retained, &ring);
    if (err != BB_OK) {
        bb_event_routes_port_unlock(s_topics_lock);
        bb_log_e(TAG, "ring attach failed for '%s': %d", topic_name, err);
        return err;
    }

    // Next free slot: safe to compute from the registry's count because
    // s_topics_lock (held for this whole function) serialises every
    // attach_ex2 call, and the registry is register-only (no deregister),
    // so its count always equals the number of slots already committed.
    attached_topic_t *t = &s_topics[bb_event_topic_registry_count()];
    strncpy(t->name, topic_name, TOPIC_NAME_MAX - 1);
    t->name[TOPIC_NAME_MAX - 1] = '\0';
    t->topic = topic;
    t->ring = ring;

    err = bb_event_topic_registry_register(t->name, t);
    if (err != BB_OK) {  // LCOV_EXCL_BR_LINE — count/dedupe checks above (find_by_handle miss + count < cap) make this unreachable in practice.
        // LCOV_EXCL_START
        bb_event_routes_port_unlock(s_topics_lock);
        bb_log_e(TAG, "registry register failed for '%s': %d", topic_name, err);
        return err;
        // LCOV_EXCL_STOP
    }
    bb_event_routes_port_unlock(s_topics_lock);

    bb_log_d(TAG, "attached '%s' retained=%d max_entry=%zu", topic_name,
             (int)retained, entry_size);
    return BB_OK;
}

bb_err_t bb_event_routes_attach_ex(const char *topic_name, bool retained)
{
    return bb_event_routes_attach_ex2(topic_name, retained, 0);
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
#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL || defined(BB_EVENT_ROUTES_TESTING)
            if (use_static_pool()) {
                // Validate that runtime cfg fits within the compile-time pool dimensions.
                if (c->queue_depth > CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH ||
                    c->max_entry > CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY) {
                    atomic_store(&c->in_use, false);
                    return BB_ERR_NO_SPACE;
                }
                // Assign from static pool — zero the slots since they're reused.
                c->entries = s_entry_pool[i];
                c->payload_buf = s_payload_pool[i];
                memset(c->entries, 0, c->queue_depth * sizeof(queue_entry_t));
                memset(c->payload_buf, 0, c->queue_depth * c->max_entry);
            } else {
#endif
                c->entries = (queue_entry_t *)s_calloc(c->queue_depth, sizeof(queue_entry_t));
                c->payload_buf = (uint8_t *)s_calloc(c->queue_depth, c->max_entry);
#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL || defined(BB_EVENT_ROUTES_TESTING)
            }
#endif
            c->port_lock = bb_event_routes_port_lock_create();
            c->event = bb_event_routes_port_event_create();
            if (!c->entries || !c->payload_buf || !c->port_lock || !c->event) {  // LCOV_EXCL_BR_LINE — port_lock alloc covered by entries/payload paths
#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL || defined(BB_EVENT_ROUTES_TESTING)
                if (!use_static_pool()) {  // LCOV_EXCL_BR_LINE — static pool entries never NULL
                    s_free(c->entries);
                    s_free(c->payload_buf);
                }
#else
                s_free(c->entries);
                s_free(c->payload_buf);
#endif
                if (c->port_lock) bb_event_routes_port_lock_destroy(c->port_lock);  // LCOV_EXCL_BR_LINE
                if (c->event) bb_event_routes_port_event_destroy(c->event);  // LCOV_EXCL_BR_LINE — paired with port_lock alloc-failure path
                c->entries = NULL;
                c->payload_buf = NULL;
                c->port_lock = NULL;
                c->event = NULL;
                atomic_store(&c->in_use, false);
                return BB_ERR_NO_SPACE;
            }

            // Snapshot the topic table under the lock so concurrent attach_ex
            // calls cannot race the iteration below.
            typedef struct { bb_event_ring_t ring; char name[TOPIC_NAME_MAX]; } topic_snap_t;
            topic_snap_t snaps[CONFIG_BB_EVENT_ROUTES_MAX_TOPICS];
            size_t snap_count;
            bb_event_routes_port_lock(s_topics_lock);
            snap_count = bb_event_topic_registry_count();
            for (size_t k = 0; k < snap_count; k++) {
                snaps[k].ring = s_topics[k].ring;
                memcpy(snaps[k].name, s_topics[k].name, TOPIC_NAME_MAX);
            }
            bb_event_routes_port_unlock(s_topics_lock);

            // Subscribe to matching topics. Replay-on-connect via ring.
            for (size_t k = 0; k < snap_count; k++) {
                // Filter to topic_filter if specified.
                if (topic_filter && strcmp(snaps[k].name, topic_filter) != 0) {
                    continue;
                }
                bb_event_sub_t sub = NULL;
                bb_err_t err = bb_event_ring_subscribe_with_replay(
                    snaps[k].ring, capture_cb, c, &sub);
                if (err != BB_OK) {
                    // Roll back: unsubscribe what we already did, release.
                    for (size_t j = 0; j < c->num_subs; j++) {
                        bb_event_unsubscribe(c->subs[j]);
                    }
#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL || defined(BB_EVENT_ROUTES_TESTING)
                    if (!use_static_pool()) {
                        s_free(c->entries);
                        s_free(c->payload_buf);
                    }
#else
                    s_free(c->entries);
                    s_free(c->payload_buf);
#endif
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
#if CONFIG_BB_EVENT_ROUTES_STATIC_POOL || defined(BB_EVENT_ROUTES_TESTING)
    if (!use_static_pool()) {
        s_free(c->entries);
        s_free(c->payload_buf);
    }
#else
    s_free(c->entries);
    s_free(c->payload_buf);
#endif
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

    // Read topic name under the topics lock; copy to local so serialization
    // is lock-free.  capture_cb guarantees topic_idx is always valid.
    char topic_name[TOPIC_NAME_MAX];
    bb_event_routes_port_lock(s_topics_lock);
    memcpy(topic_name, s_topics[e.topic_idx].name, TOPIC_NAME_MAX);
    bb_event_routes_port_unlock(s_topics_lock);
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

int bb_event_routes_client_slot_index(const bb_event_routes_client_t *c)
{
    if (!c) return -1;
    ptrdiff_t idx = c - s_clients;
    if (idx < 0 || (size_t)idx >= CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS) return -1;
    return (int)idx;
}

// ---------------------------------------------------------------------------
// Diagnostics accessors (public, no ESP-IDF deps)
// ---------------------------------------------------------------------------

size_t bb_event_routes_topic_count(void)
{
    return bb_event_topic_registry_count();
}

bb_err_t bb_event_routes_topic_info(size_t idx,
                                    const char **name,
                                    bb_event_ring_t *ring)
{
    attached_topic_t *t = NULL;
    bb_err_t err = bb_event_topic_registry_get_by_index(idx, &t);
    if (err != BB_OK) {
        return err;
    }
    if (name) *name = t->name;
    if (ring) *ring = t->ring;
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
    size_t num_topics = bb_event_topic_registry_count();
    for (size_t i = 0; i < num_topics; i++) {
        attached_topic_t *t = NULL;
        if (bb_event_topic_registry_get_by_index(i, &t) == BB_OK && t->ring) {  // LCOV_EXCL_BR_LINE
            bb_event_ring_detach(t->ring);
        }
    }
    bb_event_topic_registry_test_reset();
    memset(&s_cfg, 0, sizeof(s_cfg));
    if (s_topics_lock) {
        bb_event_routes_port_lock_destroy(s_topics_lock);
        s_topics_lock = NULL;
    }
    s_use_static_pool_for_test = false;
}

size_t bb_event_routes_queued_for_test(bb_event_routes_client_t *c) {
    return c ? c->count : 0;  // LCOV_EXCL_BR_LINE
}

uint64_t bb_event_routes_dropped_for_test(bb_event_routes_client_t *c) {
    return c ? c->dropped : 0;  // LCOV_EXCL_BR_LINE
}
#endif
