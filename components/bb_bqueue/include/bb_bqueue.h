// bb_bqueue — blocking queue: mailbox (capacity==1) or bounded MPSC
// (capacity>1) on the SAME opaque type. Mirrors FreeRTOS's own idiom:
// xQueueCreate(1, ...) + xQueueOverwrite() IS the documented mailbox on a
// plain QueueHandle_t — the espidf backend wraps xQueue directly, so
// splitting this into two components would fabricate a distinction the
// wrapped platform does not have (B1-821).
//
/**
 * @brief Blocking mailbox/MPSC queue — capacity==1 selects mailbox mode
 * (overwrite/reset), capacity>1 selects bounded-MPSC mode (send/dropped);
 * peek/receive/count/capacity work in both. Zero heap: a Kconfig-sized
 * static instance pool.
 */
//
// Mode enforcement (not merely documented — checked at every call):
//   overwrite() / reset() on a capacity>1 queue -> BB_ERR_INVALID_STATE
//   send() / dropped() on a capacity==1 queue   -> BB_ERR_INVALID_STATE
//
// peek() is ALWAYS non-consuming in both modes, and safe for any number of
// concurrent peekers -- this is the property TaipanMiner's work_queue
// depends on (two concurrent peekers, mining.c + asic_task.c, queue never
// drained). The host backend wakes ALL blocked peekers on overwrite()/send()
// via bb_lock_cond_broadcast() (never signal(), which would starve every
// waiter but one) -- see platform/host/bb_bqueue/bb_bqueue.c. The espidf
// backend's xQueuePeek() is natively safe for N concurrent peekers per
// FreeRTOS's own contract; no broadcast equivalent needed there.
//
// timeout_ms == 0 is a non-blocking try: peek()/receive() on empty ->
// BB_ERR_NOT_FOUND; send() at capacity -> BB_ERR_NO_SPACE (and dropped()
// increments). A genuine timeout expiry (timeout_ms > 0) always reports
// BB_ERR_TIMEOUT.
//
// Usage (mailbox, e.g. TaipanMiner's work_queue):
//   bb_bqueue_cfg_t cfg = { .capacity = 1, .item_bytes = sizeof(work_t), .name = "work" };
//   bb_bqueue_t q;
//   bb_bqueue_create(&cfg, &q);
//   bb_bqueue_overwrite(q, &work);              // producer, never blocks
//   bb_bqueue_peek(q, &work, BB_BQUEUE_WAIT_FOREVER); // consumer(s), non-consuming
//
// Usage (bounded MPSC, e.g. TaipanMiner's result_queue):
//   bb_bqueue_cfg_t cfg = { .capacity = 8, .item_bytes = sizeof(result_t), .name = "result" };
//   bb_bqueue_create(&cfg, &q);
//   bb_bqueue_send(q, &result, 100);            // producer, blocks up to 100ms
//   bb_bqueue_receive(q, &result, BB_BQUEUE_WAIT_FOREVER); // single consumer, consuming
#pragma once

#include "bb_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// timeout_ms sentinel: block with no timeout. Must match
// BB_LOCK_COND_WAIT_FOREVER's semantics (bb_lock.h).
#define BB_BQUEUE_WAIT_FOREVER 0xFFFFFFFFu

// Opaque handle — never dereference directly. Backed by a Kconfig-sized
// static instance pool (BB_BQUEUE_MAX_INSTANCES); no heap allocation.
typedef struct bb_bqueue *bb_bqueue_t;

/**
 * Configuration for bb_bqueue_create.
 *
 * capacity   — 1 selects mailbox mode; >1 selects bounded-MPSC mode. Must be
 *              <= Kconfig BB_BQUEUE_MAX_CAPACITY.
 * item_bytes — fixed item size in bytes. Must be <= Kconfig
 *              BB_BQUEUE_MAX_ITEM_BYTES.
 * name       — optional human-readable identifier; not persisted or queried
 *              back (diagnostic/caller-side use only), may be NULL.
 */
typedef struct {
    size_t      capacity;
    size_t      item_bytes;
    const char *name;
} bb_bqueue_cfg_t;

/**
 * Acquire an instance from the static pool.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if cfg/out is NULL, or
 *         cfg->capacity/item_bytes is 0 or exceeds the Kconfig maxima;
 *         BB_ERR_NO_SPACE if the static instance pool
 *         (BB_BQUEUE_MAX_INSTANCES) is exhausted.
 */
bb_err_t bb_bqueue_create(const bb_bqueue_cfg_t *cfg, bb_bqueue_t *out);

/**
 * Release the instance back to the static pool. The caller MUST quiesce
 * every producer/consumer/peeker on q first — destroying a queue with a
 * thread still blocked in peek()/receive()/send() is undefined behavior
 * (same contract as bb_lock_cond_destroy). Safe to call with NULL.
 */
void bb_bqueue_destroy(bb_bqueue_t q);

// ---------------------------------------------------------------------------
// Mailbox mode (capacity == 1)
// ---------------------------------------------------------------------------

/**
 * Overwrite the mailbox's single slot. Never blocks, never reports "full" —
 * always succeeds. Wakes every blocked peek()/receive() waiter.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if q/item is NULL;
 *         BB_ERR_INVALID_STATE if q was created with capacity > 1.
 */
bb_err_t bb_bqueue_overwrite(bb_bqueue_t q, const void *item);

/**
 * Clear the mailbox's slot (a subsequent peek()/receive() with timeout_ms=0
 * returns BB_ERR_NOT_FOUND until the next overwrite()).
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if q is NULL;
 *         BB_ERR_INVALID_STATE if q was created with capacity > 1.
 */
bb_err_t bb_bqueue_reset(bb_bqueue_t q);

// ---------------------------------------------------------------------------
// Bounded-MPSC mode (capacity > 1)
// ---------------------------------------------------------------------------

/**
 * Enqueue item, blocking up to timeout_ms if full (BB_BQUEUE_WAIT_FOREVER to
 * block indefinitely; 0 to try once and fail immediately). Wakes every
 * blocked peek()/receive() waiter on success.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if q/item is NULL;
 *         BB_ERR_INVALID_STATE if q was created with capacity == 1;
 *         BB_ERR_NO_SPACE if full and timeout_ms == 0 (dropped() increments);
 *         BB_ERR_TIMEOUT if still full when timeout_ms elapses (dropped()
 *         increments).
 */
bb_err_t bb_bqueue_send(bb_bqueue_t q, const void *item, uint32_t timeout_ms);

/**
 * Number of send() calls that failed to enqueue (immediate NO_SPACE or
 * timeout) since bb_bqueue_create().
 *
 * @return BB_OK with *out_dropped set; BB_ERR_INVALID_ARG if q/out_dropped
 *         is NULL; BB_ERR_INVALID_STATE if q was created with capacity == 1.
 */
bb_err_t bb_bqueue_dropped(bb_bqueue_t q, size_t *out_dropped);

// ---------------------------------------------------------------------------
// Both modes
// ---------------------------------------------------------------------------

/**
 * Copy the current item into *out_item WITHOUT consuming it. Blocks up to
 * timeout_ms while empty (BB_BQUEUE_WAIT_FOREVER to block indefinitely; 0 to
 * try once and fail immediately). Safe for any number of concurrent callers
 * — see the header-level note above.
 *
 * @return BB_OK with *out_item set; BB_ERR_INVALID_ARG if q/out_item is
 *         NULL; BB_ERR_NOT_FOUND if empty and timeout_ms == 0;
 *         BB_ERR_TIMEOUT if still empty when timeout_ms elapses.
 */
bb_err_t bb_bqueue_peek(bb_bqueue_t q, void *out_item, uint32_t timeout_ms);

/**
 * Copy the current item into *out_item and consume it (mailbox: clears the
 * slot; MPSC: pops the oldest item, waking a blocked send() waiter). Blocks
 * up to timeout_ms while empty (BB_BQUEUE_WAIT_FOREVER to block
 * indefinitely; 0 to try once and fail immediately).
 *
 * @return BB_OK with *out_item set; BB_ERR_INVALID_ARG if q/out_item is
 *         NULL; BB_ERR_NOT_FOUND if empty and timeout_ms == 0;
 *         BB_ERR_TIMEOUT if still empty when timeout_ms elapses.
 */
bb_err_t bb_bqueue_receive(bb_bqueue_t q, void *out_item, uint32_t timeout_ms);

/** Current number of items held (0 or 1 for a mailbox). 0 for a NULL/invalid handle. */
size_t bb_bqueue_count(bb_bqueue_t q);

/** The capacity q was created with. 0 for a NULL/invalid handle. */
size_t bb_bqueue_capacity(bb_bqueue_t q);

// ---------------------------------------------------------------------------
// Host test hooks (only when BB_BQUEUE_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_BQUEUE_TESTING

/** Reset every pooled instance (test isolation). */
void bb_bqueue_test_reset(void);

/**
 * Number of callers currently blocked inside a bb_lock_cond_wait() call on
 * q's not-empty/not-full condition (host backend only — always 0 on
 * espidf/arduino, which do not expose this). Used by tests to prove a
 * waiter is GENUINELY parked (not merely "about to call peek()") before
 * signalling it, without a sleep-based race.
 */
size_t bb_bqueue_test_waiting_count(bb_bqueue_t q);

#endif /* BB_BQUEUE_TESTING */

#ifdef __cplusplus
}
#endif
