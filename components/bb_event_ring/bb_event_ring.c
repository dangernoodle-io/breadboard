#include "bb_event_ring.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>

// Clock abstraction: returns microseconds since an arbitrary epoch.
// On ESP-IDF the platform component (platform/espidf/bb_event_ring/) provides
// a strong definition backed by esp_timer_get_time(). On Arduino and POSIX
// hosts the portable definition below is used.
int64_t bb_event_ring_now_us(void);

#ifndef ESP_PLATFORM
#if defined(ARDUINO)
#include <Arduino.h>
int64_t bb_event_ring_now_us(void) { return (int64_t)micros(); }
#else
#define _POSIX_C_SOURCE 200112L
#include <time.h>
int64_t bb_event_ring_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}
#endif
#endif // !ESP_PLATFORM

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
    bool   retained;  // Marks topic as state-not-events; see bb_event_ring_attach_ex docs.

    // Ring buffer: array of entries, each followed by max_entry bytes for payload
    bb_event_ring_entry_t *entries;
    uint8_t *payload_buf;

    size_t head;   // Next write position
    size_t tail;   // Oldest entry position (when full)
    size_t count;  // Number of entries in ring

    // Diagnostics: metadata of the most recent captured entry (0 if none)
    int64_t last_post_us;
    size_t  last_size;

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

    // Serialize against subscribe_with_replay's snapshot, which takes the same lock.
    bb_event_lock();

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

    // Record diagnostics for the most recent entry.
    ring->last_post_us = bb_event_ring_now_us();
    ring->last_size    = size;

    // Update count and tail BEFORE advancing head: if we're evicting, we evict tail BEFORE we move to next position
    if (ring->count < ring->capacity) {
        ring->count++;
    } else {
        // Already full; evict oldest entry by advancing tail
        ring->tail = (ring->tail + 1) % ring->capacity;
    }

    // Advance head to next write position
    ring->head = (ring->head + 1) % ring->capacity;

    bb_event_unlock();
}

bb_err_t bb_event_ring_attach_ex(bb_event_topic_t topic, size_t capacity,
                                 size_t max_entry, bool retained,
                                 bb_event_ring_t *out)
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
    ring->retained = retained;
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
    bb_log_i(TAG, "attached ring: capacity=%zu max_entry=%zu retained=%d",
             capacity, max_entry, (int)retained);
    return BB_OK;
}

bb_err_t bb_event_ring_attach(bb_event_topic_t topic, size_t capacity,
                              size_t max_entry, bb_event_ring_t *out)
{
    return bb_event_ring_attach_ex(topic, capacity, max_entry, false, out);
}

// Snapshot entry metadata (header). Payloads are stored separately in a flat buffer.
typedef struct {
    int32_t id;
    size_t size;
} snapshot_header_t;

typedef struct {
    bb_event_ring_t ring;
    snapshot_header_t *headers;
    uint8_t *payloads;
    size_t max_entry;
    size_t snap_count;
} snapshot_ctx_t;

static void snapshot_prep(void *arg)
{
    snapshot_ctx_t *ctx = (snapshot_ctx_t *)arg;
    bb_event_ring_t ring = ctx->ring;

    // Walk entries from oldest to newest into the pre-allocated snapshot buffer.
    for (size_t i = 0; i < ring->count; i++) {
        size_t idx = (ring->tail + i) % ring->capacity;
        ctx->headers[i].id = ring->entries[idx].id;
        ctx->headers[i].size = ring->entries[idx].size;
        if (ctx->headers[i].size > 0) {
            memcpy(ctx->payloads + (i * ctx->max_entry),
                   ring->payload_buf + (idx * ring->max_entry),
                   ctx->headers[i].size);
        }
    }
    ctx->snap_count = ring->count;
}

bb_err_t bb_event_ring_subscribe_with_replay(bb_event_ring_t ring,
                                             bb_event_handler_fn cb, void *user,
                                             bb_event_sub_t *out_sub)
{
    if (!ring || !cb || !out_sub) {
        return BB_ERR_INVALID_ARG;
    }

    // Pre-allocate worst-case snapshot buffers before taking the lock. Using
    // ring->capacity (not ->count) avoids racing with ring_capture for the size.
    // Allocate two buffers: headers and payloads (right-sized to max_entry, not fixed 512).
    snapshot_header_t *headers = (snapshot_header_t *)s_calloc(ring->capacity, sizeof(*headers));
    if (!headers) {
        bb_log_e(TAG, "failed to allocate snapshot headers");
        return BB_ERR_NO_SPACE;
    }

    uint8_t *payloads = (uint8_t *)s_calloc(ring->capacity, ring->max_entry);
    if (!payloads) {
        bb_log_e(TAG, "failed to allocate snapshot payloads");
        s_free(headers);
        return BB_ERR_NO_SPACE;
    }

    snapshot_ctx_t ctx = { .ring = ring, .headers = headers, .payloads = payloads,
                           .max_entry = ring->max_entry, .snap_count = 0 };

    bb_err_t err = bb_event_subscribe_with_prep(ring->topic, cb, user,
                                                snapshot_prep, &ctx, out_sub);
    if (err != BB_OK) {
        s_free(payloads);
        s_free(headers);
        bb_log_e(TAG, "failed to subscribe");
        return err;
    }

    // Replay snapshot (outside lock, so new events post naturally)
    for (size_t i = 0; i < ctx.snap_count; i++) {
        cb(ring->topic, headers[i].id, payloads + (i * ring->max_entry), headers[i].size, user);
    }

    size_t snap_count = ctx.snap_count;
    s_free(payloads);
    s_free(headers);
    bb_log_d(TAG, "replayed %zu entries", snap_count);
    return BB_OK;
}

size_t bb_event_ring_capacity(bb_event_ring_t ring)
{
    if (!ring) return 0;
    // capacity is set at attach time and never changes — no lock needed.
    return ring->capacity;
}

size_t bb_event_ring_count(bb_event_ring_t ring)
{
    if (!ring) return 0;
    bb_event_lock();
    size_t n = ring->count;
    bb_event_unlock();
    return n;
}

bb_err_t bb_event_ring_last_entry_info(bb_event_ring_t ring,
                                       uint32_t *id,
                                       size_t *size,
                                       int64_t *post_us)
{
    if (!ring) return BB_ERR_INVALID_ARG;

    bb_event_lock();
    if (ring->count == 0) {
        bb_event_unlock();
        return BB_ERR_NOT_FOUND;
    }
    // Most recent entry is at (head - 1) mod capacity.
    size_t last_idx = (ring->head + ring->capacity - 1) % ring->capacity;
    uint32_t entry_id   = (uint32_t)ring->entries[last_idx].id;
    size_t   entry_size = ring->entries[last_idx].size;
    int64_t  entry_us   = ring->last_post_us;
    bb_event_unlock();

    if (id)      *id      = entry_id;
    if (size)    *size    = entry_size;
    if (post_us) *post_us = entry_us;
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
