// Must come before any system header (which transitively pulls features.h
// and locks _POSIX_C_SOURCE at the glibc default). Needed so <time.h> below
// exposes clock_gettime + CLOCK_MONOTONIC on the POSIX host backend.
#if !defined(ESP_PLATFORM) && !defined(ARDUINO) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include "bb_event_ring.h"
#include "bb_ring.h"
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

// ---------------------------------------------------------------------------
// Local allocator pointers.
//
// bb_event_ring_set_allocator() sets BOTH:
//   1. bb_ring's allocator (via bb_ring_set_allocator) — covers entry storage.
//   2. s_snap_calloc/free here — covers temporary snapshot buffers in
//      subscribe_with_replay (pre-existing test contracts rely on this).
//
// On ESP-IDF, bb_ring_espidf installs the SPIRAM allocator at EARLY tier via
// bb_ring_set_allocator directly.  bb_event_ring_spiram.c (now a no-op shim)
// no longer calls bb_event_ring_set_allocator, so s_snap_calloc stays at the
// system default (calloc) and snapshot temp buffers land in internal heap —
// fine because they are short-lived and small (capacity × max_entry × 2).
// ---------------------------------------------------------------------------

typedef void *(*bb_event_ring_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_event_ring_free_fn)(void *p);

static bb_event_ring_calloc_fn s_snap_calloc = calloc;
static bb_event_ring_free_fn   s_snap_free   = free;

void bb_event_ring_set_allocator(bb_event_ring_calloc_fn c, bb_event_ring_free_fn f)
{
    s_snap_calloc = c ? c : calloc;
    s_snap_free   = f ? f : free;
    // Forward to bb_ring for entry storage.
    bb_ring_set_allocator((bb_ring_calloc_fn)c, (bb_ring_free_fn)f);
}

void bb_event_ring_reset_allocator(void)
{
    s_snap_calloc = calloc;
    s_snap_free   = free;
    bb_ring_reset_allocator();
}

// ---------------------------------------------------------------------------
// Ring struct
// ---------------------------------------------------------------------------

struct bb_event_ring {
    bb_event_topic_t topic;
    size_t capacity;
    size_t max_entry;
    bool   retained;  // Marks topic as state-not-events; see bb_event_ring_attach_ex docs.

    bb_ring_t ring;   // Shared primitive: holds all entry storage.

    // Diagnostics: metadata of the most recent captured entry (0 if none).
    // last_post_us is tracked here because it's set at capture time from
    // bb_event_ring_now_us() — the timestamp in bb_ring is also set from that
    // value but we store it separately for fast retrieval in last_entry_info.
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

    int64_t now = bb_event_ring_now_us();

    // bb_ring uses uint32_t id; int32_t casts safely (bit-identical round-trip).
    // Oversized payloads are rejected by bb_ring_push (returns INVALID_ARG) —
    // the event bus enforces max_payload via bb_event_cfg_t so this should not
    // happen in practice, but the ring is still safe if it does.
    bb_err_t push_err = bb_ring_push(ring->ring, data, size, now, (uint32_t)id);
    if (push_err != BB_OK) {
        bb_log_w(TAG, "ring_capture: payload dropped (size=%zu > max_entry=%zu); "
                 "check CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY",
                 size, ring->max_entry);
    }

    // Record diagnostics for the most recent entry.
    ring->last_post_us = now;
    ring->last_size    = size;

    bb_event_unlock();
}

bb_err_t bb_event_ring_attach_ex(bb_event_topic_t topic, size_t capacity,
                                 size_t max_entry, bool retained,
                                 bb_event_ring_t *out)
{
    if (!topic || !capacity || !max_entry || !out) {
        return BB_ERR_INVALID_ARG;
    }

    // Allocate the small wrapper struct (does not hold entry storage).
    // Uses the snap allocator (same as what bb_event_ring_set_allocator sets)
    // so test-injected failing allocators cover this allocation at index 0 —
    // matching the old three-allocation sequence (struct=0, entries=1, payload=2).
    bb_event_ring_t ring = (bb_event_ring_t)s_snap_calloc(1, sizeof(*ring));
    if (!ring) {
        bb_log_e(TAG, "failed to allocate ring struct");
        return BB_ERR_NO_SPACE;
    }

    // Create the shared ring primitive — allocates entries[] + payload[] via
    // the active bb_ring allocator (SPIRAM-preferred on ESP-IDF).
    // This consumes alloc indices 1 and 2 when a test failing allocator is set.
    // Name the ring after the topic so diagnostics (bb_ring_name, future
    // telemetry registry B1-414) identify the owner without a separate map.
    bb_err_t err = bb_ring_create(capacity, max_entry, BB_RING_EVICT_OLDEST,
                                  bb_event_topic_name(topic), &ring->ring);
    if (err != BB_OK) {
        bb_log_e(TAG, "bb_ring_create failed: %d", (int)err);
        s_snap_free(ring);
        return err;
    }

    ring->topic    = topic;
    ring->capacity = capacity;
    ring->max_entry = max_entry;
    ring->retained  = retained;

    // Subscribe ring's private callback to the topic
    err = bb_event_subscribe(topic, ring_capture, (void *)ring, &ring->port_sub);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to subscribe to topic");
        bb_ring_destroy(ring->ring);
        s_snap_free(ring);
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

// ---------------------------------------------------------------------------
// Snapshot helpers for replay (non-destructive multi-subscriber replay)
// ---------------------------------------------------------------------------

typedef struct {
    bb_event_ring_t ring;
    struct {
        int32_t id;
        size_t  size;
    } *headers;
    uint8_t *payloads;
    size_t   max_entry;
    size_t   snap_count;
} snapshot_ctx_t;

static void snapshot_prep(void *arg)
{
    snapshot_ctx_t *ctx = (snapshot_ctx_t *)arg;
    bb_ring_t r = ctx->ring->ring;
    size_t count = bb_ring_count(r);

    // Walk entries oldest→newest using bb_ring_peek_at (non-destructive).
    // Other subscribers can independently replay the same entries because
    // peek_at does NOT consume (pop) anything from the ring.
    for (size_t i = 0; i < count; i++) {
        size_t   out_len = 0;
        int64_t  out_ts  = 0;
        uint32_t out_id  = 0;
        uint8_t *payload_dst = ctx->payloads + (i * ctx->max_entry);

        bb_err_t rc = bb_ring_peek_at(r, i,
                                      payload_dst, ctx->max_entry,
                                      &out_len, &out_ts, &out_id);
        if (rc != BB_OK) break;  // LCOV_EXCL_LINE — ring shrank under us

        ctx->headers[i].id   = (int32_t)out_id;
        ctx->headers[i].size = out_len;
    }
    ctx->snap_count = count;
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
    // Uses s_snap_calloc so test-injected failing allocators apply here too.
    typedef struct { int32_t id; size_t size; } hdr_t;
    hdr_t *headers = (hdr_t *)s_snap_calloc(ring->capacity, sizeof(*headers));
    if (!headers) {
        bb_log_e(TAG, "failed to allocate snapshot headers");
        return BB_ERR_NO_SPACE;
    }

    uint8_t *payloads = (uint8_t *)s_snap_calloc(ring->capacity, ring->max_entry);
    if (!payloads) {
        bb_log_e(TAG, "failed to allocate snapshot payloads");
        s_snap_free(headers);
        return BB_ERR_NO_SPACE;
    }

    snapshot_ctx_t ctx = { .ring = ring, .headers = (void *)headers,
                           .payloads = payloads,
                           .max_entry = ring->max_entry, .snap_count = 0 };

    bb_err_t err = bb_event_subscribe_with_prep(ring->topic, cb, user,
                                                snapshot_prep, &ctx, out_sub);
    if (err != BB_OK) {
        s_snap_free(payloads);
        s_snap_free(headers);
        bb_log_e(TAG, "failed to subscribe");
        return err;
    }

    // Replay snapshot (outside lock, so new events post naturally).
    // bb_ring_peek_at is non-destructive — ring is unmodified; concurrent
    // subscribers can replay the same entries independently.
    for (size_t i = 0; i < ctx.snap_count; i++) {
        cb(ring->topic, headers[i].id, payloads + (i * ring->max_entry), headers[i].size, user);
    }

    size_t snap_count = ctx.snap_count;
    s_snap_free(payloads);
    s_snap_free(headers);
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
    size_t n = bb_ring_count(ring->ring);
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
    size_t count = bb_ring_count(ring->ring);
    if (count == 0) {
        bb_event_unlock();
        return BB_ERR_NOT_FOUND;
    }

    // Most recent entry is at index (count - 1) in the ring (newest).
    size_t   out_len = 0;
    int64_t  out_ts  = 0;
    uint32_t out_id  = 0;
    bb_ring_peek_at(ring->ring, count - 1, NULL, 0, &out_len, &out_ts, &out_id);
    int64_t lpu = ring->last_post_us;
    bb_event_unlock();

    if (id)      *id      = out_id;
    if (size)    *size    = out_len;
    if (post_us) *post_us = lpu;
    return BB_OK;
}

void bb_event_ring_detach(bb_event_ring_t ring)
{
    if (!ring) return;

    // Unsubscribe from the topic
    if (ring->port_sub) {  // LCOV_EXCL_LINE
        bb_event_unsubscribe(ring->port_sub);
    }

    // Free shared ring storage
    bb_ring_destroy(ring->ring);

    // Free the small event-ring wrapper struct (allocated via s_snap_calloc)
    s_snap_free(ring);

    bb_log_i(TAG, "detached ring");
}
