// bb_ring — generic variable-length circular buffer with per-entry metadata.
//
// Compiled on both host (tests) and ESP-IDF (via bb_ring CMakeLists SRCS).
// On ESP-IDF the bb_ring_espidf platform component overrides the allocator
// at EARLY tier to prefer SPIRAM with internal-heap fallback.
//
// Layout (mirrors bb_event_ring):
//   metadata array : capacity × bb_ring_entry_t  (id, ts, payload length)
//   payload buffer : capacity × max_entry_bytes   (flat; entry i at i*max_entry)
//
// head  : next write slot (mod capacity)
// tail  : oldest entry slot (mod capacity)  — only valid when count > 0
// count : number of live entries (0 .. capacity)
//
// Thread-safety: NONE. Caller serialises access.

#include "bb_ring.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_ring";

// ---------------------------------------------------------------------------
// Allocator state (overridable for SPIRAM + test injection)
// ---------------------------------------------------------------------------

typedef void *(*calloc_fn)(size_t n, size_t sz);
typedef void  (*free_fn)(void *p);

static calloc_fn s_calloc = calloc;
static free_fn   s_free   = free;

void bb_ring_set_allocator(bb_ring_calloc_fn c, bb_ring_free_fn f)
{
    s_calloc = c ? c : calloc;
    s_free   = f ? f : free;
}

void bb_ring_reset_allocator(void)
{
    s_calloc = calloc;
    s_free   = free;
}

// ---------------------------------------------------------------------------
// Internal entry metadata
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t id;
    int64_t  ts;
    size_t   len;
} bb_ring_entry_t;

// ---------------------------------------------------------------------------
// Ring struct
// ---------------------------------------------------------------------------

struct bb_ring {
    size_t capacity;      // max entries
    size_t max_entry;     // max payload bytes per entry

    bb_ring_entry_t *entries;   // [capacity]
    uint8_t         *payload;   // [capacity * max_entry]

    size_t head;    // next write slot
    size_t tail;    // oldest entry slot
    size_t count;   // live entry count

    size_t bytes_used;   // sum of entry lengths currently stored
    size_t dropped;      // entries dropped due to ring-full (evicted or rejected)
    size_t truncated;    // push() calls rejected (len > max_entry)

    bb_ring_full_policy_t policy;  // BB_RING_EVICT_OLDEST or BB_RING_REJECT_NEW
    char name[BB_RING_NAME_MAX];   // diagnostic label (bounded copy, never borrowed)
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bb_err_t bb_ring_create(size_t capacity_entries, size_t max_entry_bytes,
                        bb_ring_full_policy_t policy, const char *name,
                        bb_ring_t *out)
{
    if (!capacity_entries || !max_entry_bytes || !out) {
        return BB_ERR_INVALID_ARG;
    }
    if (policy != BB_RING_EVICT_OLDEST && policy != BB_RING_REJECT_NEW) {
        return BB_ERR_INVALID_ARG;
    }

    bb_ring_t r = (bb_ring_t)s_calloc(1, sizeof(*r));
    if (!r) {
        bb_log_e(TAG, "failed to allocate ring struct");
        return BB_ERR_NO_SPACE;
    }

    r->entries = (bb_ring_entry_t *)s_calloc(capacity_entries, sizeof(bb_ring_entry_t));
    if (!r->entries) {
        bb_log_e(TAG, "failed to allocate entries array");
        s_free(r);
        return BB_ERR_NO_SPACE;
    }

    r->payload = (uint8_t *)s_calloc(capacity_entries, max_entry_bytes);
    if (!r->payload) {
        bb_log_e(TAG, "failed to allocate payload buffer");
        s_free(r->entries);
        s_free(r);
        return BB_ERR_NO_SPACE;
    }

    r->capacity  = capacity_entries;
    r->max_entry = max_entry_bytes;
    r->policy    = policy;
    r->head      = 0;
    r->tail      = 0;
    r->count     = 0;
    r->bytes_used  = 0;
    r->dropped     = 0;
    r->truncated   = 0;

    if (name) {
        strncpy(r->name, name, BB_RING_NAME_MAX - 1);
        r->name[BB_RING_NAME_MAX - 1] = '\0';
    } else {
        r->name[0] = '\0';
    }

    bb_log_i(TAG, "created ring '%s': capacity=%zu max_entry=%zu policy=%d",
             r->name, capacity_entries, max_entry_bytes, (int)policy);
    *out = r;
    return BB_OK;
}

void bb_ring_destroy(bb_ring_t r)
{
    if (!r) return;
    bb_log_i(TAG, "destroyed ring '%s'", r->name);
    s_free(r->payload);
    s_free(r->entries);
    s_free(r);
}

const char *bb_ring_name(bb_ring_t r)
{
    return r ? r->name : "";
}

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------

bb_err_t bb_ring_push(bb_ring_t r, const void *data, size_t len,
                      int64_t ts, uint32_t id)
{
    if (!r) return BB_ERR_INVALID_ARG;
    if (len > 0 && !data) return BB_ERR_INVALID_ARG;

    // Reject oversized entries
    if (len > r->max_entry) {
        r->truncated++;
        bb_log_w(TAG, "push rejected: len=%zu > max_entry=%zu", len, r->max_entry);
        return BB_ERR_INVALID_ARG;
    }

    size_t write_idx = r->head;

    // Handle full ring according to policy
    if (r->count == r->capacity) {
        if (r->policy == BB_RING_REJECT_NEW) {
            r->dropped++;
            bb_log_d(TAG, "push rejected: ring full (reject-new policy)");
            return BB_ERR_NO_SPACE;
        }
        // BB_RING_EVICT_OLDEST: evict oldest before writing
        r->bytes_used -= r->entries[r->tail].len;
        r->tail = (r->tail + 1) % r->capacity;
        r->dropped++;
    } else {
        r->count++;
    }

    // Write metadata
    r->entries[write_idx].id  = id;
    r->entries[write_idx].ts  = ts;
    r->entries[write_idx].len = len;

    // Write payload
    if (len > 0) {
        memcpy(r->payload + (write_idx * r->max_entry), data, len);
    }

    r->bytes_used += len;
    r->head = (r->head + 1) % r->capacity;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// FIFO read
// ---------------------------------------------------------------------------

bb_err_t bb_ring_peek_oldest(bb_ring_t r,
                             void *buf, size_t buf_cap,
                             size_t *out_len,
                             int64_t *out_ts,
                             uint32_t *out_id)
{
    if (!r || !out_len || !out_ts || !out_id) return BB_ERR_INVALID_ARG;
    if (r->count == 0) return BB_ERR_NOT_FOUND;

    bb_ring_entry_t *e = &r->entries[r->tail];
    *out_len = e->len;
    *out_ts  = e->ts;
    *out_id  = e->id;

    if (buf && buf_cap > 0 && e->len > 0) {
        size_t copy_len = e->len < buf_cap ? e->len : buf_cap;
        memcpy(buf, r->payload + (r->tail * r->max_entry), copy_len);
    }

    return BB_OK;
}

bb_err_t bb_ring_pop_oldest(bb_ring_t r)
{
    if (!r) return BB_ERR_INVALID_ARG;
    if (r->count == 0) return BB_ERR_NOT_FOUND;

    r->bytes_used -= r->entries[r->tail].len;
    r->tail = (r->tail + 1) % r->capacity;
    r->count--;
    return BB_OK;
}

bb_err_t bb_ring_peek_at(bb_ring_t r, size_t index,
                         void *buf, size_t buf_cap,
                         size_t *out_len,
                         int64_t *out_ts,
                         uint32_t *out_id)
{
    if (!r || !out_len || !out_ts || !out_id) return BB_ERR_INVALID_ARG;
    if (r->count == 0 || index >= r->count) return BB_ERR_NOT_FOUND;

    size_t slot = (r->tail + index) % r->capacity;
    bb_ring_entry_t *e = &r->entries[slot];
    *out_len = e->len;
    *out_ts  = e->ts;
    *out_id  = e->id;

    if (buf && buf_cap > 0 && e->len > 0) {
        size_t copy_len = e->len < buf_cap ? e->len : buf_cap;
        memcpy(buf, r->payload + (slot * r->max_entry), copy_len);
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

size_t bb_ring_count(bb_ring_t r)
{
    return r ? r->count : 0;
}

size_t bb_ring_bytes_used(bb_ring_t r)
{
    return r ? r->bytes_used : 0;
}

size_t bb_ring_dropped(bb_ring_t r)
{
    return r ? r->dropped : 0;
}

size_t bb_ring_truncated(bb_ring_t r)
{
    return r ? r->truncated : 0;
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

void bb_ring_clear(bb_ring_t r)
{
    if (!r) return;
    r->head      = 0;
    r->tail      = 0;
    r->count     = 0;
    r->bytes_used  = 0;
    // dropped and truncated are cumulative diagnostics — intentionally NOT
    // reset here so callers can detect losses across clear boundaries.
}
