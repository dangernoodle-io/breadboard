#pragma once

// bb_queue — generic variable-length circular buffer with per-entry metadata.
//
// A bounded FIFO ring of variable-length byte entries, each carrying a
// timestamp (int64_t, caller-supplied microseconds) and a 32-bit id.
//
// Capacity model: fixed entry-count capacity, each entry up to max_entry_bytes.
// Storage is eagerly allocated as two flat arrays:
//   - metadata array:  capacity × sizeof(bb_queue_entry_t)
//   - payload buffer:  capacity × max_entry_bytes
// (mirrors bb_event_ring's proven layout).
//
// Full-ring policy (bb_queue_full_policy_t):
//   BB_QUEUE_EVICT_OLDEST: when full, push() silently evicts the oldest entry
//     and increments the dropped counter. Returns BB_OK.
//   BB_QUEUE_REJECT_NEW: when full, push() does NOT write the new entry,
//     increments the dropped counter, and returns BB_ERR_NO_SPACE. The oldest
//     entry is preserved intact.
//
// Oversized push: payloads larger than max_entry_bytes are REJECTED (not
// truncated). bb_queue_push() returns BB_ERR_INVALID_ARG and increments the
// truncated counter. Document choice: reject is safer for binary protocols; the
// caller can always pre-truncate and push a shorter entry.
//
// Thread-safety: bb_queue is NOT internally locked. The caller is responsible
// for serialising access (FreeRTOS consumers should wrap operations in a
// mutex).
//
// SPIRAM allocation: on ESP-IDF, register the platform/espidf/bb_queue_espidf
// component (via EXTRA_COMPONENT_DIRS) — it installs a SPIRAM-preferred
// allocator at EARLY tier. On host builds the default malloc/calloc is used.

#include "bb_core.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Opaque handle
// ---------------------------------------------------------------------------

typedef struct bb_queue *bb_queue_t;

// Maximum length (including NUL) of a ring's diagnostic name.
// Names longer than BB_QUEUE_NAME_MAX - 1 are silently truncated.
#define BB_QUEUE_NAME_MAX 24

// ---------------------------------------------------------------------------
// Full-ring policy
// ---------------------------------------------------------------------------

// bb_queue_full_policy_t — controls what happens when push() is called on a
// full ring (count == capacity).
//
//   BB_QUEUE_EVICT_OLDEST : (default) silently evict the oldest entry and write
//     the new one. The dropped counter is incremented. Returns BB_OK.
//   BB_QUEUE_REJECT_NEW   : do NOT write the new entry; increment the dropped
//     counter and return BB_ERR_NO_SPACE. The oldest entry is preserved intact.
//
// Oversized-payload behavior (len > max_entry_bytes) is identical for both
// policies: BB_ERR_INVALID_ARG, truncated counter incremented, entry not written.
typedef enum {
    BB_QUEUE_EVICT_OLDEST = 0,
    BB_QUEUE_REJECT_NEW,
} bb_queue_full_policy_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// bb_queue_create — allocate a new ring with an explicit full-ring policy and
//   a diagnostic name.
//
//   capacity_entries : maximum number of entries the ring holds at once.
//   max_entry_bytes  : maximum payload size for a single entry (inclusive).
//   policy           : BB_QUEUE_EVICT_OLDEST or BB_QUEUE_REJECT_NEW.
//   name             : short diagnostic label (copied internally, truncated to
//                      BB_QUEUE_NAME_MAX - 1 chars). NULL stores "".
//   out              : receives the allocated handle on BB_OK.
//
// Returns BB_ERR_INVALID_ARG if capacity_entries or max_entry_bytes is zero,
//   out is NULL, or policy is not a recognised value.
// Returns BB_ERR_NO_SPACE    if allocation fails.
bb_err_t bb_queue_create(size_t capacity_entries, size_t max_entry_bytes,
                        bb_queue_full_policy_t policy, const char *name,
                        bb_queue_t *out);

// bb_queue_destroy — free all resources. Safe to call with NULL (no-op).
void bb_queue_destroy(bb_queue_t r);

// bb_queue_name — return the diagnostic name stored at create time.
// Returns "" when r is NULL.
const char *bb_queue_name(bb_queue_t r);

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------

// bb_queue_push — append an entry.
//
//   data  : pointer to payload bytes (may be NULL when len == 0).
//   len   : payload length in bytes.
//   ts    : caller-supplied timestamp (e.g. microseconds since boot).
//   id    : caller-supplied 32-bit identifier.
//
// If len > max_entry_bytes: returns BB_ERR_INVALID_ARG, increments truncated
// counter. Entry is NOT written (same for all policies).
//
// If ring is full (BB_QUEUE_EVICT_OLDEST): oldest entry is evicted before
// writing, dropped counter incremented. Returns BB_OK.
//
// If ring is full (BB_QUEUE_REJECT_NEW): entry is NOT written, dropped counter
// incremented. Returns BB_ERR_NO_SPACE. The oldest entry is preserved intact.
//
// Returns BB_ERR_INVALID_ARG if r is NULL or (len > 0 && data is NULL).
bb_err_t bb_queue_push(bb_queue_t r, const void *data, size_t len,
                      int64_t ts, uint32_t id);

// ---------------------------------------------------------------------------
// FIFO read — peek/pop pattern for replay-then-remove consumers
// ---------------------------------------------------------------------------

// bb_queue_peek_oldest — copy the oldest entry into caller-supplied buffer.
//
// On BB_OK:
//   *out_len  receives the entry's payload length (may be 0).
//   *out_ts   receives the entry's timestamp.
//   *out_id   receives the entry's id.
//   buf       receives up to min(out_len, buf_cap) payload bytes.
//
// Returns BB_ERR_NOT_FOUND if ring is empty.
// Returns BB_ERR_INVALID_ARG if r or out_len or out_ts or out_id is NULL.
// Note: if buf_cap < *out_len, only buf_cap bytes are copied. The caller can
// pass buf=NULL + buf_cap=0 to probe id/ts/len without copying.
bb_err_t bb_queue_peek_oldest(bb_queue_t r,
                             void *buf, size_t buf_cap,
                             size_t *out_len,
                             int64_t *out_ts,
                             uint32_t *out_id);

// bb_queue_pop_oldest — remove the oldest entry WITHOUT copying.
// Returns BB_ERR_NOT_FOUND if ring is empty.
// Returns BB_ERR_INVALID_ARG if r is NULL.
bb_err_t bb_queue_pop_oldest(bb_queue_t r);

// bb_queue_peek_at — NON-DESTRUCTIVE read of entry at logical index.
//
//   index 0 = oldest entry; index (count-1) = newest.
//
// On BB_OK:
//   *out_len  receives the entry's payload length.
//   *out_ts   receives the entry's timestamp.
//   *out_id   receives the entry's id.
//   buf       receives up to min(*out_len, buf_cap) bytes when buf != NULL.
//
// Returns BB_ERR_NOT_FOUND if ring is empty or index >= count.
// Returns BB_ERR_INVALID_ARG if r or out_len or out_ts or out_id is NULL.
//
// Does NOT consume (pop) the entry — designed for replay-without-drain use
// (e.g. bb_event_ring replay-on-subscribe: iterate all N entries without
// removing them so other subscribers can still replay the same data).
bb_err_t bb_queue_peek_at(bb_queue_t r, size_t index,
                         void *buf, size_t buf_cap,
                         size_t *out_len,
                         int64_t *out_ts,
                         uint32_t *out_id);

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

// Current number of entries in the ring.
size_t bb_queue_count(bb_queue_t r);

// Maximum number of entries the ring holds (as passed to bb_queue_create).
// Returns 0 if r is NULL.
size_t bb_queue_capacity(bb_queue_t r);

// Total bytes of payload data currently stored (sum of all entry lengths).
size_t bb_queue_bytes_used(bb_queue_t r);

// Number of entries dropped due to ring-full eviction since creation or clear.
size_t bb_queue_dropped(bb_queue_t r);

// Number of push() calls rejected because len > max_entry_bytes.
size_t bb_queue_truncated(bb_queue_t r);

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

// bb_queue_clear — discard all entries. Does NOT free memory.
//
// Resets structural state (head, tail, count, bytes_used) so the ring is
// empty and ready for reuse. The dropped and truncated counters are
// intentionally preserved — they are cumulative diagnostics and survive
// clear boundaries so callers can detect losses across resets.
void bb_queue_clear(bb_queue_t r);

// ---------------------------------------------------------------------------
// Allocator hook — for SPIRAM override (ESP-IDF) and failure injection (tests)
// ---------------------------------------------------------------------------
// The ESP-IDF SPIRAM platform shim (bb_queue_espidf) calls these at EARLY tier
// to redirect allocations to SPIRAM-preferred heap with internal fallback.
// Tests may call them to inject a failing allocator.
typedef void *(*bb_queue_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_queue_free_fn)(void *p);
void bb_queue_set_allocator(bb_queue_calloc_fn c, bb_queue_free_fn f);
void bb_queue_reset_allocator(void);

#ifdef __cplusplus
}
#endif
