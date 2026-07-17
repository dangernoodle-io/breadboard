// bb_queue — generic variable-length circular buffer with per-entry metadata.
//
// Compiled on both host (tests) and ESP-IDF (via bb_queue CMakeLists SRCS).
// On ESP-IDF the bb_queue_espidf platform component overrides the allocator
// at EARLY tier to prefer SPIRAM with internal-heap fallback.
//
// Layout (mirrors bb_event_ring):
//   metadata array : capacity × bb_queue_entry_t  (id, ts, payload length)
//   payload buffer : capacity × max_entry_bytes   (flat; entry i at i*max_entry)
//
// head  : next write slot (mod capacity)
// tail  : oldest entry slot (mod capacity)  — only valid when count > 0
// count : number of live entries (0 .. capacity)
//
// Thread-safety: NONE. Caller serialises access.

#include "bb_queue.h"
#include "bb_queue_registry.h"
#include "bb_age.h"
#include "bb_log.h"
#include "bb_str.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_queue";

// Subtracts `len` from `*bytes_used`, clamping at 0 instead of wrapping.
// bytes_used is decremented on every pop/evict; push/pop accounting is kept
// symmetric by construction, but this guards against any future accounting
// drift (or a malformed entry) surfacing as a ~4.29e9 (u32-narrowed) value
// on /api/diag/rings instead of a clean 0.
static void bb_queue_bytes_used_sub(size_t *bytes_used, size_t len)
{
    *bytes_used = (*bytes_used >= len) ? (*bytes_used - len) : 0;
}

// ---------------------------------------------------------------------------
// Allocator state (overridable for SPIRAM + test injection)
// ---------------------------------------------------------------------------

typedef void *(*calloc_fn)(size_t n, size_t sz);
typedef void  (*free_fn)(void *p);

static calloc_fn s_calloc = calloc;
static free_fn   s_free   = free;

void bb_queue_set_allocator(bb_queue_calloc_fn c, bb_queue_free_fn f)
{
    s_calloc = c ? c : calloc;
    s_free   = f ? f : free;
}

void bb_queue_reset_allocator(void)
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
} bb_queue_entry_t;

// ---------------------------------------------------------------------------
// Ring struct
// ---------------------------------------------------------------------------

struct bb_queue {
    size_t capacity;      // max entries
    size_t max_entry;     // max payload bytes per entry

    bb_queue_entry_t *entries;   // [capacity]
    uint8_t         *payload;   // [capacity * max_entry]

    size_t head;    // next write slot
    size_t tail;    // oldest entry slot
    size_t count;   // live entry count

    size_t bytes_used;   // sum of entry lengths currently stored
    size_t dropped;      // entries dropped due to ring-full (evicted or rejected)
    size_t truncated;    // push() calls rejected (len > max_entry)

    size_t   max_bytes;   // total payload-byte budget; 0 = disabled (B1-1031)
    uint32_t max_age;     // max entry age (caller-defined unit); 0 = disabled (B1-1031)

    bb_queue_full_policy_t policy;  // BB_QUEUE_EVICT_OLDEST or BB_QUEUE_REJECT_NEW
    char name[BB_QUEUE_NAME_MAX];   // diagnostic label (bounded copy, never borrowed)
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bb_err_t bb_queue_create_ex(const bb_queue_cfg_t *cfg, bb_queue_t *out)
{
    if (!cfg || !cfg->capacity_entries || !cfg->max_entry_bytes || !out) {
        return BB_ERR_INVALID_ARG;
    }
    if (cfg->policy != BB_QUEUE_EVICT_OLDEST && cfg->policy != BB_QUEUE_REJECT_NEW) {
        return BB_ERR_INVALID_ARG;
    }
    // A byte budget smaller than the max single-entry size can never accept
    // that entry's own max-size push -- reject the config outright rather
    // than silently NO_SPACE-ing every future max-size push.
    if (cfg->max_bytes > 0 && cfg->max_bytes < cfg->max_entry_bytes) {
        return BB_ERR_INVALID_ARG;
    }

    bb_queue_t r = (bb_queue_t)s_calloc(1, sizeof(*r));
    if (!r) {
        bb_log_e(TAG, "failed to allocate ring struct");
        return BB_ERR_NO_SPACE;
    }

    r->entries = (bb_queue_entry_t *)s_calloc(cfg->capacity_entries, sizeof(bb_queue_entry_t));
    if (!r->entries) {
        bb_log_e(TAG, "failed to allocate entries array");
        s_free(r);
        return BB_ERR_NO_SPACE;
    }

    r->payload = (uint8_t *)s_calloc(cfg->capacity_entries, cfg->max_entry_bytes);
    if (!r->payload) {
        bb_log_e(TAG, "failed to allocate payload buffer");
        s_free(r->entries);
        s_free(r);
        return BB_ERR_NO_SPACE;
    }

    r->capacity  = cfg->capacity_entries;
    r->max_entry = cfg->max_entry_bytes;
    r->policy    = cfg->policy;
    r->head      = 0;
    r->tail      = 0;
    r->count     = 0;
    r->bytes_used  = 0;
    r->dropped     = 0;
    r->truncated   = 0;
    r->max_bytes   = cfg->max_bytes;
    r->max_age     = cfg->max_age;

    if (cfg->name) {
        bb_strlcpy(r->name, cfg->name, sizeof(r->name));
    } else {
        r->name[0] = '\0';
    }

    bb_log_i(TAG, "created ring '%s': capacity=%zu max_entry=%zu policy=%d max_bytes=%zu max_age=%" PRIu32,
             r->name, r->capacity, r->max_entry, (int)r->policy, r->max_bytes, r->max_age);

    // Best-effort self-registration for GET /api/diag/rings — a full
    // registry or a duplicate name is logged (inside bb_queue_registry) and
    // does NOT fail ring creation.
    bb_queue_registry_register(r->name, r);

    *out = r;
    return BB_OK;
}

bb_err_t bb_queue_create(size_t capacity_entries, size_t max_entry_bytes,
                        bb_queue_full_policy_t policy, const char *name,
                        bb_queue_t *out)
{
    bb_queue_cfg_t cfg = {
        .capacity_entries = capacity_entries,
        .max_entry_bytes  = max_entry_bytes,
        .policy           = policy,
        .name             = name,
        .max_bytes        = 0,
        .max_age          = 0,
    };
    return bb_queue_create_ex(&cfg, out);
}

void bb_queue_destroy(bb_queue_t r)
{
    if (!r) return;
    bb_queue_registry_deregister(r);
    bb_log_i(TAG, "destroyed ring '%s'", r->name);
    s_free(r->payload);
    s_free(r->entries);
    s_free(r);
}

const char *bb_queue_name(bb_queue_t r)
{
    return r ? r->name : "";
}

// ---------------------------------------------------------------------------
// Age eviction (B1-1031)
// ---------------------------------------------------------------------------

// Evicts (oldest-first) every entry whose age relative to `now` classifies
// as BB_AGE_EVICT (stale window unused -- age eviction here is a hard cutoff
// at max_age). No-op if age eviction is disabled or the ring is empty.
// Assumes entry timestamps are non-decreasing in push order (true for any
// caller supplying a monotonic clock), so eviction can stop at the first
// entry that is not yet expired. A backward/out-of-order `now` (earlier
// than the entry's ts, from clock skew/NTP step/non-monotonic or
// multi-producer callers) is clamped to age 0 (FRESH) rather than wrapping
// negative into a huge uint64_t -- never evicts on a backward clock.
// Returns the number of entries evicted.
static size_t bb_queue_evict_aged(bb_queue_t r, int64_t now)
{
    if (r->max_age == 0) return 0;

    size_t evicted = 0;
    while (r->count > 0) {
        int64_t delta = now - r->entries[r->tail].ts;
        uint64_t age = (delta > 0) ? (uint64_t)delta : 0;
        if (bb_age_classify(age, 0, r->max_age) != BB_AGE_EVICT) {
            break;
        }
        bb_queue_bytes_used_sub(&r->bytes_used, r->entries[r->tail].len);
        r->tail = (r->tail + 1) % r->capacity;
        r->count--;
        r->dropped++;
        evicted++;
    }
    return evicted;
}

size_t bb_queue_evict_expired(bb_queue_t r, int64_t now)
{
    if (!r) return 0;
    return bb_queue_evict_aged(r, now);
}

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------

bb_err_t bb_queue_push(bb_queue_t r, const void *data, size_t len,
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

    // Age-sweep: evict anything already expired relative to this entry's own
    // timestamp before considering space for the new entry. Skipped entirely
    // when age eviction is disabled (mirrors the byte-budget guards below) --
    // bb_queue_evict_aged() early-returns on max_age==0 anyway, but calling
    // it unconditionally on every push is a needless hot-path call.
    if (r->max_age > 0) {
        bb_queue_evict_aged(r, ts);
    }

    // NOTE: an entry whose own len alone could never fit under max_bytes is
    // rejected at CONFIG time, not here -- bb_queue_create_ex() requires
    // max_bytes >= max_entry_bytes (B1-1031 review), and the truncate check
    // above already bounds len <= max_entry_bytes, so len <= max_bytes is
    // guaranteed for every entry that reaches this point. No separate
    // per-push "outright reject" is reachable/needed.

    size_t write_idx = r->head;

    // Space is exhausted if the ring is at entry-count capacity OR (when a
    // byte budget is configured) this entry would push bytes_used over it.
    int over_capacity = (r->count == r->capacity);
    int over_bytes    = (r->max_bytes > 0 && r->bytes_used + len > r->max_bytes);

    if (over_capacity || over_bytes) {
        if (r->policy == BB_QUEUE_REJECT_NEW) {
            r->dropped++;
            bb_log_d(TAG, "push rejected: ring full (reject-new policy)");
            return BB_ERR_NO_SPACE;
        }
        // BB_QUEUE_EVICT_OLDEST: evict oldest entries until both predicates
        // are false. Bounded by r->count -- an empty ring always satisfies
        // both (count != capacity>0, bytes_used==0 <= max_bytes since
        // len <= max_bytes is guaranteed, see the NOTE above).
        while (r->count > 0 &&
               (r->count == r->capacity ||
                (r->max_bytes > 0 && r->bytes_used + len > r->max_bytes))) {
            bb_queue_bytes_used_sub(&r->bytes_used, r->entries[r->tail].len);
            r->tail = (r->tail + 1) % r->capacity;
            r->count--;
            r->dropped++;
        }
    }

    r->count++;

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

bb_err_t bb_queue_peek_oldest(bb_queue_t r,
                             void *buf, size_t buf_cap,
                             size_t *out_len,
                             int64_t *out_ts,
                             uint32_t *out_id)
{
    if (!r || !out_len || !out_ts || !out_id) return BB_ERR_INVALID_ARG;
    if (r->count == 0) return BB_ERR_NOT_FOUND;

    bb_queue_entry_t *e = &r->entries[r->tail];
    *out_len = e->len;
    *out_ts  = e->ts;
    *out_id  = e->id;

    if (buf && buf_cap > 0 && e->len > 0) {
        size_t copy_len = e->len < buf_cap ? e->len : buf_cap;
        memcpy(buf, r->payload + (r->tail * r->max_entry), copy_len);
    }

    return BB_OK;
}

bb_err_t bb_queue_pop_oldest(bb_queue_t r)
{
    if (!r) return BB_ERR_INVALID_ARG;
    if (r->count == 0) return BB_ERR_NOT_FOUND;

    bb_queue_bytes_used_sub(&r->bytes_used, r->entries[r->tail].len);
    r->tail = (r->tail + 1) % r->capacity;
    r->count--;
    return BB_OK;
}

bb_err_t bb_queue_peek_at(bb_queue_t r, size_t index,
                         void *buf, size_t buf_cap,
                         size_t *out_len,
                         int64_t *out_ts,
                         uint32_t *out_id)
{
    if (!r || !out_len || !out_ts || !out_id) return BB_ERR_INVALID_ARG;
    if (r->count == 0 || index >= r->count) return BB_ERR_NOT_FOUND;

    size_t slot = (r->tail + index) % r->capacity;
    bb_queue_entry_t *e = &r->entries[slot];
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

size_t bb_queue_count(bb_queue_t r)
{
    return r ? r->count : 0;
}

size_t bb_queue_capacity(bb_queue_t r)
{
    return r ? r->capacity : 0;
}

size_t bb_queue_bytes_used(bb_queue_t r)
{
    return r ? r->bytes_used : 0;
}

size_t bb_queue_dropped(bb_queue_t r)
{
    return r ? r->dropped : 0;
}

size_t bb_queue_truncated(bb_queue_t r)
{
    return r ? r->truncated : 0;
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

void bb_queue_clear(bb_queue_t r)
{
    if (!r) return;
    r->head      = 0;
    r->tail      = 0;
    r->count     = 0;
    r->bytes_used  = 0;
    // dropped and truncated are cumulative diagnostics — intentionally NOT
    // reset here so callers can detect losses across clear boundaries.
}

// ---------------------------------------------------------------------------
// Test hooks (BB_QUEUE_TESTING)
// ---------------------------------------------------------------------------

#ifdef BB_QUEUE_TESTING
#include "bb_queue_test.h"

void bb_queue_test_force_bytes_used(bb_queue_t r, size_t value)
{
    if (!r) return;
    r->bytes_used = value;
}
#endif
