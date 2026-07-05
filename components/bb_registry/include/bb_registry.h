#pragma once
// bb_registry — generic name→handle object registry.
//
// A bb_registry_t is a fixed-capacity map from const char* name to void* value.
// All operations are lock-guarded (pthread_mutex_t, POSIX on host and ESP-IDF).
//
// Declare a registry in file scope using the macro:
//
//   BB_REGISTRY_DEFINE(s_my_reg, 16);
//   BB_REGISTRY_DEFINE_TAGGED(s_my_reg, 16, "my_component");
//
// The macro expands to a static BSS array for the entries and a static
// bb_registry_t initialised at compile time — zero heap cost.
// Capacity must not exceed BB_REGISTRY_SNAPSHOT_MAX (enforced by _Static_assert
// in the declare macros; keep capacities small, O(tens) not O(hundreds)).
//
// Thread-safety: all public functions are lock-guarded.
// foreach copy-out: the lock is dropped before invoking callbacks; do NOT call
// register/deregister from within a foreach callback (stale snapshot — newly
// registered entries are not visited this pass; deregistered entries are still
// visited via the snapshot).

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>  // ESP-IDF + POSIX host only; no Arduino/AVR backend

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Snapshot limit — foreach uses a fixed stack buffer of this size.
// All registry capacities must be <= this value (enforced by _Static_assert).
// ---------------------------------------------------------------------------

#define BB_REGISTRY_SNAPSHOT_MAX 64

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    void       *value;
} bb_registry_entry_t;

typedef struct {
    bb_registry_entry_t *entries;
    uint16_t             capacity;
    uint16_t             count;
    bool                 frozen;
    bool                 hwm_warned;
    pthread_mutex_t      lock;
    const char          *tag;
} bb_registry_t;

// ---------------------------------------------------------------------------
// Declare macro — BSS array + static registry, zero heap.
// Capacity must be <= BB_REGISTRY_SNAPSHOT_MAX.
// ---------------------------------------------------------------------------

#define BB_REGISTRY_DEFINE(var_, cap_)                                         \
    _Static_assert((cap_) <= BB_REGISTRY_SNAPSHOT_MAX,                        \
                   "bb_registry capacity exceeds BB_REGISTRY_SNAPSHOT_MAX");   \
    static bb_registry_entry_t _##var_##_entries[(cap_)];                      \
    static bb_registry_t var_ = {                                              \
        .entries  = _##var_##_entries,                                         \
        .capacity = (cap_),                                                    \
        .lock     = PTHREAD_MUTEX_INITIALIZER,                                 \
    }

#define BB_REGISTRY_DEFINE_TAGGED(var_, cap_, tag_)                            \
    _Static_assert((cap_) <= BB_REGISTRY_SNAPSHOT_MAX,                        \
                   "bb_registry capacity exceeds BB_REGISTRY_SNAPSHOT_MAX");   \
    static bb_registry_entry_t _##var_##_entries[(cap_)];                      \
    static bb_registry_t var_ = {                                              \
        .entries  = _##var_##_entries,                                         \
        .capacity = (cap_),                                                    \
        .lock     = PTHREAD_MUTEX_INITIALIZER,                                 \
        .tag      = (tag_),                                                    \
    }

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

// Register a name→value pair.
// Returns BB_ERR_INVALID_ARG if name or value is NULL.
// Returns BB_ERR_INVALID_STATE if the registry is frozen or name is duplicate.
// Returns BB_ERR_NO_SPACE if count == capacity.
// Emits a one-time HWM bb_log_w when count transitions to capacity-1
// (one slot still free), so callers have notice before the registry fills.
bb_err_t bb_registry_register(bb_registry_t *r, const char *name, void *value);

// Remove the entry with the given name (compact left, no tombstones).
// Returns BB_ERR_INVALID_ARG if name is NULL.
// Returns BB_ERR_NOT_FOUND if the name is not present.
// Returns BB_ERR_INVALID_STATE if the registry is frozen.
bb_err_t bb_registry_deregister(bb_registry_t *r, const char *name);

// Freeze the registry. Idempotent. After freezing, register/deregister
// return BB_ERR_INVALID_STATE.
void bb_registry_freeze(bb_registry_t *r);

// Iterate all entries. Acquires the lock, snapshots entries to a fixed-size
// stack buffer (BB_REGISTRY_SNAPSHOT_MAX entries), releases the lock, then
// invokes cb for each snapshot entry.
// Safe to call from multiple threads concurrently; do NOT call
// register/deregister from within cb (stale snapshot — newly registered
// entries are not visited this pass; deregistered entries are still visited
// via the snapshot).
// WARNING: on a pointer-keyed instance (see below) `name` is an opaque
// identity pointer, NOT a valid C string — do not %s/strcmp/strlen it.
// Use bb_registry_foreach_ptr on pointer-keyed instances instead.
void bb_registry_foreach(bb_registry_t *r,
                         void (*cb)(const char *name, void *value, void *ctx),
                         void *ctx);

// Return the current entry count (lock-guarded).
uint16_t bb_registry_count(bb_registry_t *r);

// Copy the entry at index idx into *out (lock-guarded).
// Returns BB_ERR_INVALID_ARG if out is NULL.
// Returns BB_ERR_NOT_FOUND if idx >= count.
// WARNING: on a pointer-keyed instance (see below) out->name is an opaque
// identity pointer, NOT a valid C string — do not %s/strcmp/strlen it.
bb_err_t bb_registry_get_by_index(bb_registry_t *r, uint16_t idx,
                                   bb_registry_entry_t *out);

// Return the value for name, or NULL if not found (lock-guarded).
// Safe on frozen registries; on live registries the caller is responsible
// for ensuring the value lifetime outlasts use of the returned pointer.
// Prefer foreach for churn consumers.
void *bb_registry_lookup(bb_registry_t *r, const char *name);

// ---------------------------------------------------------------------------
// Pointer-keyed variant
//
// A bb_registry_t instance is EITHER name-keyed (bb_registry_register/
// _deregister/_lookup/_foreach, comparing .name via strcmp) OR pointer-keyed
// (bb_registry_register_ptr/_deregister_ptr/_lookup_ptr/_foreach_ptr,
// comparing .name via == as an opaque identity pointer) — NEVER BOTH. The
// .name field is reinterpreted as a `void *` identity key for this variant;
// callers must not mix the two families of calls on the same instance.
// Storage, lock, capacity, and HWM plumbing are shared unchanged; use
// bb_registry_foreach_ptr (not bb_registry_foreach) to iterate a
// pointer-keyed instance so the key is never misread as a C string.
// ---------------------------------------------------------------------------

// Register a key->value pair, keyed by pointer identity (== comparison).
// Returns BB_ERR_INVALID_ARG if key or value is NULL.
// Returns BB_ERR_INVALID_STATE if the registry is frozen or key is duplicate.
// Returns BB_ERR_NO_SPACE if count == capacity.
// Emits the same one-time HWM bb_log_w as bb_registry_register.
bb_err_t bb_registry_register_ptr(bb_registry_t *r, void *key, void *value);

// Remove the entry with the given pointer key (compact left, no tombstones).
// Returns BB_ERR_INVALID_ARG if key is NULL.
// Returns BB_ERR_NOT_FOUND if the key is not present.
// Returns BB_ERR_INVALID_STATE if the registry is frozen.
bb_err_t bb_registry_deregister_ptr(bb_registry_t *r, const void *key);

// Return the value for key, or NULL if not found (lock-guarded).
void *bb_registry_lookup_ptr(bb_registry_t *r, const void *key);

// Iterate all entries of a pointer-keyed registry. Same copy-out/lock
// semantics as bb_registry_foreach, but cb's first parameter is typed
// `void *key` (the identity pointer) instead of `const char *name` — use
// this on pointer-keyed instances so callers get compiler help and never
// mistake the key for a C string.
void bb_registry_foreach_ptr(bb_registry_t *r,
                             void (*cb)(void *key, void *value, void *ctx),
                             void *ctx);

// ---------------------------------------------------------------------------
// Test hook (gated by BB_REGISTRY_TESTING)
// ---------------------------------------------------------------------------

#ifdef BB_REGISTRY_TESTING
// Reset count, frozen, hwm_warned flags to initial state and reinit the mutex.
// Intended for test teardown only.
void bb_registry_reset(bb_registry_t *r);
#endif

#ifdef __cplusplus
}
#endif
