#pragma once
// bb_pool — generic object/memory pool carved from a bb_arena.
//
// bb_pool sits ON bb_arena (components/bb_arena) — all pool data (slot
// buffers, FIFO ring storage, free-lists) is carved from an arena via
// bb_arena_alloc. bb_pool itself never calls the global heap allocator;
// the arena it is carved from is the sole allocation site.
//
// Two ways to obtain a pool:
//   - bb_pool_create()       — caller supplies/owns the bb_arena (share one
//                               arena across several pools, or a static-BSS
//                               arena for no-PSRAM boards). bb_pool_destroy()
//                               does not touch the arena's lifetime.
//   - bb_pool_create_owned() — bb_pool internally allocates a right-sized
//                               arena (bb_pool_arena_size_needed(cfg)) via
//                               bb_arena_init_heap / bb_arena_init_spiram and
//                               owns it; bb_pool_destroy() frees it.
//
// Four modes (bb_pool_mode_t):
//   RETAINED  : capacity index-addressed fixed slots, recycle-on-update
//               (bb_pool_retained_update / bb_pool_retained_get).
//   FIFO      : ring of capacity fixed-size entries with a full-ring policy
//               (bb_pool_push / bb_pool_peek_oldest / bb_pool_pop_oldest).
//   TRANSIENT : bump-allocate from the pool's arena; bb_pool_reset() rewinds
//               it (bb_pool_alloc / bb_pool_free / bb_pool_reset).
//   SLOTS     : fixed-slot acquire/release object pool — a free-list over
//               capacity slots of max_slot_bytes each (bb_pool_acquire /
//               bb_pool_release). Slots are opaque and typed by the caller
//               (e.g. cast to conn_t* / session_t*); the caller owns
//               per-object init/teardown — bb_pool does not run any
//               acquire/release lifecycle hooks.
//
// Thread-safety: NONE. Mirrors the bb_arena contract — the caller must
// serialise concurrent access to a single bb_pool_t instance.

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"
#include "bb_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

typedef enum {
    BB_POOL_MODE_RETAINED  = 0,
    BB_POOL_MODE_FIFO      = 1,
    BB_POOL_MODE_TRANSIENT = 2,
    BB_POOL_MODE_SLOTS     = 3,
} bb_pool_mode_t;

// Controls what happens when bb_pool_push() is called on a full FIFO pool.
// FIFO mode only; ignored for other modes. Mirrors bb_ring_full_policy_t's
// shape (own type — bb_pool does not depend on bb_ring).
typedef enum {
    BB_POOL_FULL_EVICT_OLDEST = 0,
    BB_POOL_FULL_REJECT_NEW   = 1,
} bb_pool_full_policy_t;

// Backing storage for bb_pool_create_owned().
typedef enum {
    BB_POOL_BACKING_HEAP   = 0,
    BB_POOL_BACKING_SPIRAM = 1,
} bb_pool_backing_t;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

typedef struct {
    bb_pool_mode_t        mode;
    size_t                capacity;       /**< slot count (RETAINED/SLOTS) or ring depth (FIFO). Unused for TRANSIENT. */
    size_t                max_slot_bytes; /**< max bytes per slot / FIFO entry payload. Unused for TRANSIENT. */
    bb_pool_full_policy_t full_policy;    /**< FIFO only. */
    const char           *name;           /**< diagnostic label (copied, truncated). */
    /**
     * TRANSIENT mode only: bytes of bump-allocation space to reserve beyond
     * the pool struct itself, when this cfg is passed to
     * bb_pool_create_owned() (bb_pool_arena_size_needed() adds it to the
     * computed arena size). Without this, an owned TRANSIENT pool has zero
     * bump headroom and every bb_pool_alloc() call returns NULL. Ignored by
     * every other mode, and ignored by bb_pool_create() against a
     * caller-supplied arena (that arena's own size is what governs
     * available bump space there).
     */
    size_t                transient_reserve_bytes;
} bb_pool_cfg_t;

// ---------------------------------------------------------------------------
// Opaque handle
// ---------------------------------------------------------------------------

typedef struct bb_pool *bb_pool_t;

// ---------------------------------------------------------------------------
// Sizing helper
// ---------------------------------------------------------------------------

/**
 * Return the number of bytes an arena must provide to back a pool created
 * with `cfg` (includes the pool struct and all mode-specific storage —
 * SLOTS mode includes its acquired-slot bitmap, TRANSIENT mode includes
 * cfg.transient_reserve_bytes — with alignment padding). Used internally by
 * bb_pool_create() and bb_pool_create_owned(), and by callers sizing a
 * shared/static arena for bb_pool_create().
 *
 * Returns 0 for NULL cfg, zero capacity/max_slot_bytes on a mode that
 * requires them, an unrecognised mode, or on size_t overflow.
 */
size_t bb_pool_arena_size_needed(const bb_pool_cfg_t *cfg);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * Create a pool carved entirely from caller-supplied `arena`. The pool
 * struct and all mode-specific data are allocated via bb_arena_alloc — no
 * global heap calls. `arena` must have >= bb_pool_arena_size_needed(cfg)
 * free bytes. The caller retains ownership of `arena`'s lifetime;
 * bb_pool_destroy() will not free it.
 *
 * Returns BB_ERR_INVALID_ARG for NULL cfg/arena/out or invalid cfg fields.
 * Returns BB_ERR_NO_SPACE if the arena is exhausted.
 */
bb_err_t bb_pool_create(const bb_pool_cfg_t *cfg, bb_arena_t arena,
                         bb_pool_t *out);

/**
 * Create a pool that internally allocates and OWNS a right-sized arena
 * (bb_pool_arena_size_needed(cfg) bytes) via bb_arena_init_heap (HEAP) or
 * bb_arena_init_spiram (SPIRAM). bb_pool_destroy() frees the owned arena.
 * Use this form to use bb_pool without managing an arena yourself.
 *
 * Returns BB_ERR_INVALID_ARG for NULL cfg/out or invalid cfg fields.
 * Returns BB_ERR_NO_MEM if the backing allocation fails.
 */
bb_err_t bb_pool_create_owned(const bb_pool_cfg_t *cfg,
                               bb_pool_backing_t backing, bb_pool_t *out);

/**
 * Destroy a pool. For a bb_pool_create_owned() pool, frees the owned arena
 * (and therefore all pool storage, including the pool struct itself). For a
 * bb_pool_create() pool, frees nothing — the caller-supplied arena's
 * lifetime is untouched. Safe to call with NULL.
 *
 * After this call returns, `pool` is invalid for all creation forms and
 * MUST NOT be re-destroyed, reused, or dereferenced — a second
 * bb_pool_destroy() on a bb_pool_create_owned() handle is a use-after-free
 * (the freed arena block also backed the pool struct itself). Mirrors
 * bb_arena_destroy()'s documented caveat.
 */
void bb_pool_destroy(bb_pool_t pool);

/**
 * Copy the backing arena's allocation stats into *out.
 * No-op for NULL pool or NULL out.
 */
void bb_pool_get_stats(bb_pool_t pool, bb_arena_stats_t *out);

// ---------------------------------------------------------------------------
// TRANSIENT path
// ---------------------------------------------------------------------------

/** Allocate `bytes` from the pool's backing arena. NULL on exhaustion. */
void *bb_pool_alloc(bb_pool_t pool, size_t bytes);

/** Free a pointer previously allocated via bb_pool_alloc. */
void  bb_pool_free(bb_pool_t pool, void *ptr);

/** Reset the transient arena (rewind bump offset; logically frees all allocs). */
void  bb_pool_reset(bb_pool_t pool);

// ---------------------------------------------------------------------------
// RETAINED path
// ---------------------------------------------------------------------------

/**
 * Copy `len` bytes from `data` into pre-allocated slot `slot_idx`.
 * len must be <= cfg.max_slot_bytes.
 * Returns BB_ERR_INVALID_ARG if slot_idx >= capacity, len > max_slot_bytes,
 * or data is NULL while len > 0.
 * Returns BB_ERR_INVALID_STATE if pool was not created in RETAINED mode.
 *
 * A shrinking update (new len < previous len) only advances the logical
 * length forward from byte 0 to the new len — bytes past the new len from
 * the previous write are left physically present but logically unreachable
 * (bb_pool_retained_get() caps *out_len to the new len). They are not
 * zeroed.
 */
bb_err_t bb_pool_retained_update(bb_pool_t pool, size_t slot_idx,
                                  const void *data, size_t len);

/**
 * Return a read-only pointer into pre-allocated slot `slot_idx`.
 * *out_ptr and *out_len are only valid until the next retained_update on the
 * same slot, or pool destruction — do not cache the pointer.
 * Returns BB_ERR_NOT_FOUND if the slot has never been updated.
 * Returns BB_ERR_INVALID_ARG if slot_idx >= capacity or out_ptr/out_len NULL.
 * Returns BB_ERR_INVALID_STATE if pool was not created in RETAINED mode.
 */
bb_err_t bb_pool_retained_get(bb_pool_t pool, size_t slot_idx,
                               const void **out_ptr, size_t *out_len);

// ---------------------------------------------------------------------------
// FIFO path
// ---------------------------------------------------------------------------

/**
 * Push an entry into the FIFO ring.
 * If len > cfg.max_slot_bytes: returns BB_ERR_INVALID_ARG, entry not written.
 * If full (BB_POOL_FULL_EVICT_OLDEST): oldest entry is evicted first, dropped
 * counter incremented, new entry written, returns BB_OK.
 * If full (BB_POOL_FULL_REJECT_NEW): entry NOT written, dropped counter
 * incremented, returns BB_ERR_NO_SPACE.
 * Returns BB_ERR_INVALID_STATE if pool was not created in FIFO mode.
 */
bb_err_t bb_pool_push(bb_pool_t pool, const void *data, size_t len,
                       int64_t ts, uint32_t id);

/**
 * Peek at the oldest FIFO entry (non-destructive). buf receives up to
 * min(*out_len, buf_cap) payload bytes; pass buf=NULL, buf_cap=0 to probe
 * id/ts/len only. Returns BB_ERR_NOT_FOUND if the ring is empty.
 */
bb_err_t bb_pool_peek_oldest(bb_pool_t pool,
                              void *buf, size_t buf_cap,
                              size_t *out_len,
                              int64_t *out_ts,
                              uint32_t *out_id);

/** Pop (remove) the oldest FIFO entry without copying. BB_ERR_NOT_FOUND if empty. */
bb_err_t bb_pool_pop_oldest(bb_pool_t pool);

/** Current entry count in the FIFO ring. 0 for NULL pool or non-FIFO mode. */
size_t bb_pool_count(bb_pool_t pool);

/** Total entries dropped due to the full-ring policy since creation. */
size_t bb_pool_dropped(bb_pool_t pool);

// ---------------------------------------------------------------------------
// SLOTS path — fixed-slot acquire/release object pool
// ---------------------------------------------------------------------------

/**
 * Acquire a free slot. The returned pointer is aligned to at least
 * _Alignof(max_align_t) and is at least cfg.max_slot_bytes bytes — the
 * caller casts it to whatever object type it needs and owns
 * init/teardown of that object.
 * Returns NULL when the pool is exhausted (all capacity slots acquired),
 * when pool is NULL, or when pool was not created in SLOTS mode.
 */
void *bb_pool_acquire(bb_pool_t pool);

/**
 * Return a slot previously returned by bb_pool_acquire to the free-list.
 * Returns BB_ERR_INVALID_ARG if ptr does not belong to this pool's slot
 * storage, or if pool/ptr is NULL.
 * Returns BB_ERR_INVALID_STATE if pool was not created in SLOTS mode.
 */
bb_err_t bb_pool_release(bb_pool_t pool, void *ptr);

#ifdef __cplusplus
}
#endif
