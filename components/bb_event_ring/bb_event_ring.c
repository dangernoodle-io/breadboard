#include "bb_event_ring.h"
#include "bb_event_port.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_event_ring";

typedef void *(*bb_event_ring_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_event_ring_free_fn)(void *p);

static bb_event_ring_calloc_fn s_calloc = calloc;
static bb_event_ring_free_fn   s_free   = free;

#ifdef BB_EVENT_TESTING
void bb_event_ring_set_allocator(bb_event_ring_calloc_fn c, bb_event_ring_free_fn f) {
    s_calloc = c ? c : calloc;
    s_free   = f ? f : free;
}

void bb_event_ring_reset_allocator(void) {
    s_calloc = calloc;
    s_free   = free;
}
#endif

typedef struct {
    int32_t id;
    size_t size;
    uint8_t *payload;  // Pointer to payload in the ring buffer
} bb_event_ring_entry_t;

struct bb_event_ring {
    bb_event_topic_t topic;
    size_t capacity;
    size_t max_entry;

    // Ring buffer: array of entries, each followed by max_entry bytes for payload
    bb_event_ring_entry_t *entries;
    uint8_t *payload_buf;

    size_t head;   // Next write position
    size_t tail;   // Oldest entry position (when full)
    size_t count;  // Number of entries in ring

    bb_event_sub_t port_sub;  // Ring's own subscriber handle
};

// Private callback: called when an event is posted to the topic
static void ring_capture(bb_event_topic_t topic,
                        int32_t id,
                        const void *data, size_t size,
                        void *user)
{
    bb_event_ring_t ring = (bb_event_ring_t)user;
    if (!ring) return;  // LCOV_EXCL_LINE

    // bb_event_port_lock() is held during dispatch per Phase 2 design, but we take it again
    // for clarity: the ring's state is shared and we're about to modify head/tail/count.
    // The lock is recursive, so nested takes are safe.
    bb_event_port_lock();

    size_t write_idx = ring->head;

    // Copy entry metadata
    ring->entries[write_idx].id = id;
    ring->entries[write_idx].size = size;

    // Copy payload. Dispatch passes a valid pointer when size > 0
    // (payload lives in the queue slot), so the (size > 0 && !data)
    // branch is defensive only.
    if (size > 0 && data) {  // LCOV_EXCL_BR_LINE
        memcpy(ring->payload_buf + (write_idx * ring->max_entry), data, size);
    }

    // Update count and tail BEFORE advancing head: if we're evicting, we evict tail BEFORE we move to next position
    if (ring->count < ring->capacity) {
        ring->count++;
    } else {
        // Already full; evict oldest entry by advancing tail
        ring->tail = (ring->tail + 1) % ring->capacity;
    }

    // Advance head to next write position
    ring->head = (ring->head + 1) % ring->capacity;

    bb_event_port_unlock();
}

bb_err_t bb_event_ring_attach(bb_event_topic_t topic, size_t capacity,
                              size_t max_entry, bb_event_ring_t *out)
{
    if (!topic || !capacity || !max_entry || !out) {
        return BB_ERR_INVALID_ARG;
    }

    // Allocate ring struct
    bb_event_ring_t ring = (bb_event_ring_t)s_calloc(1, sizeof(*ring));
    if (!ring) {
        bb_log_e(TAG, "failed to allocate ring struct");
        return BB_ERR_NO_SPACE;
    }

    // Allocate entries array
    ring->entries = (bb_event_ring_entry_t *)s_calloc(capacity, sizeof(bb_event_ring_entry_t));
    if (!ring->entries) {
        bb_log_e(TAG, "failed to allocate entries array");
        s_free(ring);
        return BB_ERR_NO_SPACE;
    }

    // Allocate payload buffer: capacity * max_entry bytes
    ring->payload_buf = (uint8_t *)s_calloc(capacity, max_entry);
    if (!ring->payload_buf) {
        bb_log_e(TAG, "failed to allocate payload buffer");
        s_free(ring->entries);
        s_free(ring);
        return BB_ERR_NO_SPACE;
    }

    // Initialize payload pointers in entries
    for (size_t i = 0; i < capacity; i++) {
        ring->entries[i].payload = ring->payload_buf + (i * max_entry);
    }

    ring->topic = topic;
    ring->capacity = capacity;
    ring->max_entry = max_entry;
    ring->head = 0;
    ring->tail = 0;
    ring->count = 0;

    // Subscribe ring's private callback to the topic
    bb_err_t err = bb_event_subscribe(topic, ring_capture, (void *)ring, &ring->port_sub);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to subscribe to topic");
        s_free(ring->payload_buf);
        s_free(ring->entries);
        s_free(ring);
        return err;
    }

    *out = ring;
    bb_log_i(TAG, "attached ring: capacity=%zu max_entry=%zu", capacity, max_entry);
    return BB_OK;
}

bb_err_t bb_event_ring_subscribe_with_replay(bb_event_ring_t ring,
                                             bb_event_handler_fn cb, void *user,
                                             bb_event_sub_t *out_sub)
{
    if (!ring || !cb || !out_sub) {
        return BB_ERR_INVALID_ARG;
    }

    bb_event_port_lock();

    // Snapshot current buffered entries (oldest to newest)
    typedef struct {
        int32_t id;
        size_t size;
        uint8_t payload[512];  // Temp: assume max_entry <= 512 for snapshot
    } snapshot_entry_t;

    snapshot_entry_t *snapshot = NULL;
    size_t snap_count = 0;

    if (ring->count > 0) {
        snapshot = (snapshot_entry_t *)s_calloc(ring->count, sizeof(*snapshot));
        if (!snapshot) {
            bb_event_port_unlock();
            bb_log_e(TAG, "failed to allocate snapshot");
            return BB_ERR_NO_SPACE;
        }

        // Walk entries from oldest to newest
        for (size_t i = 0; i < ring->count; i++) {
            size_t idx = (ring->tail + i) % ring->capacity;
            snapshot[i].id = ring->entries[idx].id;
            snapshot[i].size = ring->entries[idx].size;
            if (snapshot[i].size > 0) {
                memcpy(snapshot[i].payload, ring->payload_buf + (idx * ring->max_entry),
                       snapshot[i].size);
            }
        }
        snap_count = ring->count;
    }

    // Subscribe for live events while still holding the lock
    bb_err_t err = bb_event_subscribe(ring->topic, cb, user, out_sub);

    bb_event_port_unlock();

    if (err != BB_OK) {
        s_free(snapshot);
        bb_log_e(TAG, "failed to subscribe");
        return err;
    }

    // Replay snapshot (outside lock, so new events post naturally)
    for (size_t i = 0; i < snap_count; i++) {
        cb(ring->topic, snapshot[i].id, snapshot[i].payload, snapshot[i].size, user);
    }

    s_free(snapshot);
    bb_log_d(TAG, "replayed %zu entries", snap_count);
    return BB_OK;
}

void bb_event_ring_detach(bb_event_ring_t ring)
{
    if (!ring) return;

    // Unsubscribe from the topic
    if (ring->port_sub) {  // LCOV_EXCL_LINE
        bb_event_unsubscribe(ring->port_sub);
    }

    // Free resources
    s_free(ring->payload_buf);
    s_free(ring->entries);
    s_free(ring);

    bb_log_i(TAG, "detached ring");
}
