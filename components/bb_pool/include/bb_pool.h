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
// SLOTS mode — optional per-slot lifecycle callbacks (B1-479)
//
// All four are optional (NULL = today's behavior, fully backward-compatible)
// and apply to SLOTS mode only. bb_pool stays portable — these are plain
// function pointers the caller supplies; any FreeRTOS/platform-specific work
// (e.g. eTaskGetState, vTaskDelete) belongs in the caller's callback bodies,
// never inside bb_pool itself.
//
//   on_acquire(ctx, slot)   — called synchronously, once, immediately before
//                             bb_pool_acquire() returns `slot` to the caller
//                             (reset-on-handout).
//   on_release(ctx, slot)   — called synchronously, once, inside
//                             bb_pool_release(), before the slot is either
//                             returned to the free-list (no slot_reusable) or
//                             marked pending-reap (slot_reusable set)
//                             (teardown-on-return).
//   slot_reusable(ctx, slot)— async reuse-readiness predicate. If NULL, a
//                             released slot is immediately eligible for
//                             reissue (today's synchronous behavior). If
//                             non-NULL, a released slot is held
//                             pending-reap instead of going onto the
//                             free-list; bb_pool_acquire(), once the
//                             free-list is empty, calls this once per
//                             pending slot (no internal loop/retry/timeout)
//                             to test whether it may be reissued.
//   slot_reap(ctx, slot)    — finalizer invoked exactly once, right before
//                             reissue, the moment slot_reusable() first
//                             returns true for that slot. Ignored if
//                             slot_reusable is NULL.
//   on_destroy(ctx, slot)   — invoked by bb_pool_destroy() over EVERY slot
//                             (regardless of free/acquired/pending state)
//                             before the backing arena is freed, so callers
//                             can release per-slot resources created lazily
//                             during the pool's life (e.g. a lazily-created
//                             OS handle stashed on first acquire). Iterates
//                             all `capacity` slots by index, not just the
//                             free-list or pending set — a slot sitting on
//                             the free-list or held pending-reap may still
//                             hold such a resource. Optional; NULL (the
//                             default) is a safe no-op — existing callers
//                             see no behavior change.
//
// bb_pool_acquire() never blocks and never retries internally: if the
// free-list is empty and no pending slot's slot_reusable() currently returns
// true, it returns NULL immediately (same contract as today's
// pool-exhausted case) — callers needing a wait/retry loop implement it
// themselves outside bb_pool.
// ---------------------------------------------------------------------------

typedef void (*bb_pool_on_acquire_fn)(void *ctx, void *slot);
typedef void (*bb_pool_on_release_fn)(void *ctx, void *slot);
typedef bool (*bb_pool_slot_reusable_fn)(void *ctx, void *slot);
typedef void (*bb_pool_slot_reap_fn)(void *ctx, void *slot);
typedef void (*bb_pool_on_destroy_fn)(void *ctx, void *slot);

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

    /**
     * SLOTS mode only: optional per-slot lifecycle callbacks — see the
     * documentation block above. All NULL (the zero value) reproduces
     * today's behavior exactly. Ignored by every other mode. `cb_ctx` is
     * passed through unchanged as the first argument to every callback.
     */
    void                    *cb_ctx;
    bb_pool_on_acquire_fn    on_acquire;
    bb_pool_on_release_fn    on_release;
    bb_pool_slot_reusable_fn slot_reusable;
    bb_pool_slot_reap_fn     slot_reap;

    /**
     * SLOTS mode only: optional per-slot finalizer invoked by
     * bb_pool_destroy() over EVERY slot (regardless of free/acquired/pending
     * state) before the backing arena is freed — see the documentation block
     * above. NULL (the default) is a safe no-op; backward-compatible with
     * every existing SLOTS-mode consumer.
     */
    bb_pool_on_destroy_fn    on_destroy;
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
 * Destroy a pool. If cfg.on_destroy is non-NULL (SLOTS mode only), it is
 * invoked once per slot over ALL `capacity` slots — regardless of
 * free/acquired/pending state — before any arena storage is freed. For a
 * bb_pool_create_owned() pool, frees the owned arena (and therefore all pool
 * storage, including the pool struct itself). For a bb_pool_create() pool,
 * frees nothing — the caller-supplied arena's lifetime is untouched. Safe to
 * call with NULL.
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
 *
 * If cfg.slot_reusable was NULL at create time, this only ever draws from
 * the free-list (today's behavior). If cfg.slot_reusable was supplied and
 * the free-list is empty, this additionally makes one non-blocking pass
 * over slots released-but-not-yet-reusable, calling slot_reusable(ctx,
 * slot) once per slot; the first slot for which it returns true has
 * slot_reap(ctx, slot) invoked and is reissued.
 *
 * If cfg.on_acquire was supplied, it is called on the returned slot
 * immediately before this function returns (after slot_reap, when taken).
 *
 * Returns NULL when no slot is currently available (all capacity slots
 * acquired, or released slots not yet reusable), when pool is NULL, or when
 * pool was not created in SLOTS mode. Never blocks.
 */
void *bb_pool_acquire(bb_pool_t pool);

/**
 * Return a slot previously returned by bb_pool_acquire. If cfg.on_release
 * was supplied, it is called first (teardown-on-return). If
 * cfg.slot_reusable was NOT supplied, the slot is then placed on the
 * free-list immediately (today's behavior). If cfg.slot_reusable WAS
 * supplied, the slot is instead held pending — not reissuable — until a
 * future bb_pool_acquire() call finds slot_reusable(ctx, slot) true.
 *
 * Returns BB_ERR_INVALID_ARG if ptr does not belong to this pool's slot
 * storage, or if pool/ptr is NULL.
 * Returns BB_ERR_INVALID_STATE if pool was not created in SLOTS mode, or if
 * ptr is not currently on loan (double-release / release of an unacquired
 * slot).
 */
bb_err_t bb_pool_release(bb_pool_t pool, void *ptr);

/**
 * B1-492: proactive garbage-collection pass over SLOTS-mode pending
 * (released-but-not-yet-reusable) slots, WITHOUT acquiring/handing any of
 * them out. For every slot currently in the pending state, calls
 * cfg.slot_reusable(ctx, slot) once; each slot for which it returns true has
 * cfg.slot_reap(ctx, slot) invoked exactly once (same finalizer bb_pool_acquire
 * would have called on reissue) and is then moved onto the free-list — NOT
 * returned to the caller. Slots for which slot_reusable returns false are
 * left pending, untouched.
 *
 * This is the idle-reclaim counterpart to bb_pool_acquire()'s inline
 * single-slot reap: a consumer with no pending acquire request (e.g. a
 * periodic housekeeping tick checking whether a pool has gone fully idle)
 * uses this to drain every corpse that has become ready, then inspects
 * bb_pool_slots_pending_count() to see whether any are still outstanding
 * before deciding whether the pool itself can be torn down.
 *
 * No-op (returns 0) for NULL pool, non-SLOTS mode, or a SLOTS pool created
 * without cfg.slot_reusable (nothing is ever pending in that configuration).
 * Never blocks.
 *
 * Returns the number of slots reaped and moved to the free-list.
 */
size_t bb_pool_slots_reap_ready(bb_pool_t pool);

/**
 * Count of SLOTS-mode slots currently pending (released, but not yet
 * confirmed reusable by cfg.slot_reusable). Always 0 for NULL pool,
 * non-SLOTS mode, or a SLOTS pool created without cfg.slot_reusable.
 */
size_t bb_pool_slots_pending_count(bb_pool_t pool);

/**
 * B1-492: count of SLOTS-mode slots currently acquired (on loan — handed out
 * by bb_pool_acquire() and not yet returned via bb_pool_release()). A
 * consumer deciding whether an entire SLOTS pool has gone idle enough to
 * bb_pool_destroy() MUST check this is 0 in addition to
 * bb_pool_slots_pending_count() == 0: a slot mid-loan (its owner has not yet
 * called bb_pool_release, e.g. still mid-teardown) is neither on the
 * free-list nor pending — destroying the pool's backing arena while such a
 * slot is still in live use elsewhere (e.g. a FreeRTOS task still executing
 * on its stack) is a use-after-free. Always 0 for NULL pool or non-SLOTS
 * mode.
 */
size_t bb_pool_slots_acquired_count(bb_pool_t pool);

#ifdef __cplusplus
}
#endif
