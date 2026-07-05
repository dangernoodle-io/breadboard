#pragma once
// bb_collection — humble ordered collection of caller-owned opaque items.
//
// A bb_collection_t stores {name, item, order} entries in a fixed-capacity
// BSS array (no heap). `item` is a CALLER-OWNED pointer — bb_collection
// never dereferences it, only stores and hands it back. Thread-safe: one
// pthread_mutex_t per collection (same primitive as bb_registry, ESP-IDF +
// POSIX host only; no Arduino/AVR backend).
//
// Ordering semantics: enumeration is order-agnostic from the collection's
// own perspective — it is a store, not a scheduler. Ordering is a
// CONSTRUCTION property expressed via the `order` field supplied at add()
// time. bb_collection_foreach() stable-sorts by `order` ascending (ties
// keep insertion order) and invokes the callback in that order, but the
// collection itself has no notion of "current position" or live reordering.
//
// This is a MECHANISM — store + ordered iterate — NOT a resolve-by-type
// autowire resolver. It does not look up entries by type, does not wire
// dependencies, and does not select an implementation for a consumer.
// That is bb_registry's job today (name/pointer -> value lookup) and a
// future bb_autowire's job for type-based resolution. If bb_collection ever
// grows resolve-by-type lookup, that re-triggers the bb_autowire governance
// review (KB#670/#673) — don't casually bolt it on here.
//
// Design note / deliberately unwired seam: an unordered-storage variant
// (skip the sort, iterate in raw slot order) would be a small addition if a
// consumer ever needs enumeration without the stable-sort cost. Nothing in
// breadboard needs it today — do not build it speculatively.

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>  // ESP-IDF + POSIX host only; no Arduino/AVR backend

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Snapshot limit — foreach uses a fixed stack buffer of this size (no heap,
// no VLA). All collection capacities must be <= this value (enforced by
// _Static_assert in BB_COLLECTION_DEFINE). Mirrors bb_registry's
// BB_REGISTRY_SNAPSHOT_MAX convention.
// ---------------------------------------------------------------------------

#define BB_COLLECTION_SNAPSHOT_MAX 64

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    const void *item;
    int         order;
} bb_collection_entry_t;

typedef struct {
    bb_collection_entry_t *entries;
    size_t                 capacity;
    size_t                 count;
    pthread_mutex_t        lock;
} bb_collection_t;

// Callback invoked once per entry by bb_collection_foreach(), in
// order-sorted sequence, without the collection lock held.
typedef void (*bb_collection_cb_t)(const bb_collection_entry_t *entry, void *ctx);

// ---------------------------------------------------------------------------
// Declare macro — BSS array + static collection, zero heap.
// ---------------------------------------------------------------------------

#define BB_COLLECTION_DEFINE(var_, cap_)                                   \
    _Static_assert((cap_) <= BB_COLLECTION_SNAPSHOT_MAX,                   \
                   "bb_collection capacity exceeds BB_COLLECTION_SNAPSHOT_MAX"); \
    static bb_collection_entry_t _##var_##_entries[(cap_)];                \
    static bb_collection_t var_ = {                                        \
        .entries  = _##var_##_entries,                                     \
        .capacity = (cap_),                                                \
        .lock     = PTHREAD_MUTEX_INITIALIZER,                             \
    }

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

// Append an entry. `item` is stored by pointer only; bb_collection never
// derefs it and the caller retains ownership/lifetime responsibility.
// Returns BB_ERR_INVALID_ARG if c or name is NULL.
// Returns BB_ERR_NO_SPACE if count == capacity (loud, fail-fast — mirrors
// bb_cache's KEY_MAX policy: no silent drop, no overwrite).
bb_err_t bb_collection_add(bb_collection_t *c, const char *name,
                            const void *item, int order);

// Iterate every entry in ascending-`order` sequence (stable: entries with
// equal `order` keep insertion order). The entry set is snapshotted to a
// fixed-size stack buffer (BB_COLLECTION_SNAPSHOT_MAX entries — no heap, no
// VLA) under the lock, sorted, the lock is released, then cb is invoked once
// per entry lock-free — item pointers are caller-owned and stable across the
// callback, so this is safe even if cb runs long. Do NOT call
// bb_collection_add from within cb (stale snapshot).
// No-op (cb never invoked) if c or cb is NULL, or the collection is empty.
void bb_collection_foreach(bb_collection_t *c, bb_collection_cb_t cb, void *ctx);

// Return the current entry count (lock-guarded). Returns 0 if c is NULL.
size_t bb_collection_count(bb_collection_t *c);

// ---------------------------------------------------------------------------
// Test hook (gated by BB_COLLECTION_TESTING)
// ---------------------------------------------------------------------------

#ifdef BB_COLLECTION_TESTING
// Reset count to zero and reinit the mutex. Intended for test teardown only.
void bb_collection_reset(bb_collection_t *c);
#endif

#ifdef __cplusplus
}
#endif
