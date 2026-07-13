// bb_bqueue — host backend: real cross-thread blocking, built on bb_lock_t
// + bb_lock_cond_t (bb_core). Backed by a Kconfig-sized static instance pool
// (BB_BQUEUE_MAX_INSTANCES); zero heap.
//
// TWO CONCURRENT PEEKERS is the property this file exists to get right
// (TaipanMiner's work_queue: two concurrent peekers, mining.c + asic_task.c,
// never drained). overwrite() and send() wake waiters via
// bb_lock_cond_broadcast() — NEVER signal(), which wakes only one waiter and
// would starve every other blocked peeker until its own timeout (see
// bb_lock.h's own header-level warning on this). Every wait loop below
// re-checks its predicate after each wake (spurious wakeups are possible)
// and uses the absolute-deadline/remaining-time idiom bb_lock_cond_wait()'s
// documentation mandates (see bb_bqueue_priv.h's
// bb_bqueue_deadline_compute()/bb_bqueue_deadline_remaining_ms()) — never
// the naive "re-arm the full timeout on every spurious wakeup" loop, which
// silently turns a bounded wait unbounded.
#include "bb_bqueue.h"
#include "bb_bqueue_priv.h"
#include "bb_clock.h"
#include "bb_lock.h"

#include <string.h>
#ifdef BB_BQUEUE_TESTING
#include <stdatomic.h>
#endif

typedef struct bb_bqueue {
    bool     in_use;
    size_t   capacity;
    size_t   item_bytes;

    bb_lock_t      lock;
    bb_lock_cond_t not_empty;  // broadcast on overwrite()/send() success
    bb_lock_cond_t not_full;   // broadcast on receive() success (MPSC only)

    uint8_t  storage[BB_BQUEUE_MAX_CAPACITY][BB_BQUEUE_MAX_ITEM_BYTES];
    size_t   head;     // index of the oldest item (MPSC ring buffer; unused in mailbox mode)
    size_t   count;    // items currently held (0 or 1 in mailbox mode)
    size_t   dropped;  // MPSC only; guarded by `lock`

#ifdef BB_BQUEUE_TESTING
    _Atomic size_t waiting_count;
#endif
} bb_bqueue_inst_t;

static bb_bqueue_inst_t s_pool[BB_BQUEUE_MAX_INSTANCES];

static bb_bqueue_inst_t *inst_from_handle(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = (bb_bqueue_inst_t *)q;
    if (!inst || !inst->in_use) return NULL;
    return inst;
}

#ifdef BB_BQUEUE_TESTING
static void wait_hook_enter(bb_bqueue_inst_t *inst) { atomic_fetch_add(&inst->waiting_count, 1); }
static void wait_hook_leave(bb_bqueue_inst_t *inst) { atomic_fetch_sub(&inst->waiting_count, 1); }
#else
static void wait_hook_enter(bb_bqueue_inst_t *inst) { (void)inst; }
static void wait_hook_leave(bb_bqueue_inst_t *inst) { (void)inst; }
#endif

// Caller MUST hold inst->lock. Blocks on `cond` while `count == blocked_at`,
// re-checking the predicate after every wake (spurious or real) using the
// absolute-deadline/remaining-time idiom. immediate_err is returned without
// blocking when timeout_ms == 0 and the predicate does not already hold.
static bb_err_t bb_bqueue_wait_until(bb_bqueue_inst_t *inst, bb_lock_cond_t *cond,
                                      size_t blocked_at, uint32_t timeout_ms, bb_err_t immediate_err)
{
    if (inst->count != blocked_at) {
        return BB_OK;
    }
    if (timeout_ms == 0) {
        return immediate_err;
    }
    if (timeout_ms == BB_BQUEUE_WAIT_FOREVER) {
        while (inst->count == blocked_at) {
            wait_hook_enter(inst);
            bb_lock_cond_wait(cond, &inst->lock, BB_LOCK_COND_WAIT_FOREVER);
            wait_hook_leave(inst);
        }
        return BB_OK;
    }

    uint64_t deadline_us = bb_bqueue_deadline_compute(bb_clock_now_us(), timeout_ms);
    while (inst->count == blocked_at) {
        uint32_t remaining_ms;
        // The expired branch here is only reachable via a SECOND loop
        // iteration whose bb_lock_cond_wait() returned BB_OK (a spurious
        // wakeup, predicate still unsatisfied) landing after the deadline --
        // a genuine race, not host-deterministically reproducible without a
        // test-only wait() stub. The FIRST iteration's not-yet-expired path
        // is exercised by every real timeout test below; every backend's
        // own comparable races (e.g. bb_lock_cond.c's pthread_cond_wait
        // failure paths) are excluded the same way.
        if (!bb_bqueue_deadline_remaining_ms(deadline_us, bb_clock_now_us(), &remaining_ms)) { // LCOV_EXCL_BR_LINE
            return BB_ERR_TIMEOUT; // LCOV_EXCL_LINE
        }
        wait_hook_enter(inst);
        bb_err_t wrc = bb_lock_cond_wait(cond, &inst->lock, remaining_ms);
        wait_hook_leave(inst);
        if (wrc == BB_ERR_TIMEOUT) {
            return BB_ERR_TIMEOUT;
        }
    }
    return BB_OK;
}

bb_err_t bb_bqueue_create(const bb_bqueue_cfg_t *cfg, bb_bqueue_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    bb_err_t vrc = bb_bqueue_validate_cfg(cfg);
    if (vrc != BB_OK) return vrc;

    int idx = -1;
    for (int i = 0; i < BB_BQUEUE_MAX_INSTANCES; i++) {
        if (!s_pool[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return BB_ERR_NO_SPACE;

    bb_bqueue_inst_t *inst = &s_pool[idx];
    memset(inst, 0, sizeof(*inst));
    inst->capacity = cfg->capacity;
    inst->item_bytes = cfg->item_bytes;

    bb_lock_init(NULL, &inst->lock);
    bb_lock_cond_init(&inst->not_empty);
    bb_lock_cond_init(&inst->not_full);

    inst->in_use = true;
    *out = (bb_bqueue_t)inst;
    return BB_OK;
}

void bb_bqueue_destroy(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst) return;

    bb_lock_cond_destroy(&inst->not_empty);
    bb_lock_cond_destroy(&inst->not_full);
    bb_lock_destroy(&inst->lock);
    memset(inst, 0, sizeof(*inst));
}

bb_err_t bb_bqueue_overwrite(bb_bqueue_t q, const void *item)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !item) return BB_ERR_INVALID_ARG;
    if (inst->capacity != 1) return BB_ERR_INVALID_STATE;

    bb_lock_lock(&inst->lock);
    memcpy(inst->storage[0], item, inst->item_bytes);
    inst->count = 1;
    // broadcast, never signal: any number of peekers may be blocked on an
    // empty mailbox and ALL must observe this write (see file header).
    bb_lock_cond_broadcast(&inst->not_empty);
    bb_lock_unlock(&inst->lock);
    return BB_OK;
}

bb_err_t bb_bqueue_reset(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->capacity != 1) return BB_ERR_INVALID_STATE;

    bb_lock_lock(&inst->lock);
    inst->count = 0;
    bb_lock_unlock(&inst->lock);
    return BB_OK;
}

bb_err_t bb_bqueue_send(bb_bqueue_t q, const void *item, uint32_t timeout_ms)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !item) return BB_ERR_INVALID_ARG;
    if (inst->capacity == 1) return BB_ERR_INVALID_STATE;

    bb_lock_lock(&inst->lock);
    bb_err_t rc = bb_bqueue_wait_until(inst, &inst->not_full, inst->capacity, timeout_ms, BB_ERR_NO_SPACE);
    if (rc != BB_OK) {
        inst->dropped++;
        bb_lock_unlock(&inst->lock);
        return rc;
    }

    size_t tail = (inst->head + inst->count) % inst->capacity;
    memcpy(inst->storage[tail], item, inst->item_bytes);
    inst->count++;
    // broadcast: peek()/receive() document N-safe concurrent callers in
    // both modes, so any number may be blocked on an empty MPSC queue too.
    bb_lock_cond_broadcast(&inst->not_empty);
    bb_lock_unlock(&inst->lock);
    return BB_OK;
}

bb_err_t bb_bqueue_dropped(bb_bqueue_t q, size_t *out_dropped)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !out_dropped) return BB_ERR_INVALID_ARG;
    if (inst->capacity == 1) return BB_ERR_INVALID_STATE;

    bb_lock_lock(&inst->lock);
    *out_dropped = inst->dropped;
    bb_lock_unlock(&inst->lock);
    return BB_OK;
}

bb_err_t bb_bqueue_peek(bb_bqueue_t q, void *out_item, uint32_t timeout_ms)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !out_item) return BB_ERR_INVALID_ARG;

    bb_lock_lock(&inst->lock);
    bb_err_t rc = bb_bqueue_wait_until(inst, &inst->not_empty, 0, timeout_ms, BB_ERR_NOT_FOUND);
    if (rc != BB_OK) {
        bb_lock_unlock(&inst->lock);
        return rc;
    }
    memcpy(out_item, inst->storage[inst->head], inst->item_bytes);
    bb_lock_unlock(&inst->lock);
    return BB_OK;
}

bb_err_t bb_bqueue_receive(bb_bqueue_t q, void *out_item, uint32_t timeout_ms)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !out_item) return BB_ERR_INVALID_ARG;

    bb_lock_lock(&inst->lock);
    bb_err_t rc = bb_bqueue_wait_until(inst, &inst->not_empty, 0, timeout_ms, BB_ERR_NOT_FOUND);
    if (rc != BB_OK) {
        bb_lock_unlock(&inst->lock);
        return rc;
    }
    memcpy(out_item, inst->storage[inst->head], inst->item_bytes);

    if (inst->capacity == 1) {
        inst->count = 0;
    } else {
        inst->head = (inst->head + 1) % inst->capacity;
        inst->count--;
        bb_lock_cond_broadcast(&inst->not_full);
    }
    bb_lock_unlock(&inst->lock);
    return BB_OK;
}

size_t bb_bqueue_count(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst) return 0;

    bb_lock_lock(&inst->lock);
    size_t n = inst->count;
    bb_lock_unlock(&inst->lock);
    return n;
}

size_t bb_bqueue_capacity(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    return inst ? inst->capacity : 0;
}

#ifdef BB_BQUEUE_TESTING

void bb_bqueue_test_reset(void)
{
    for (int i = 0; i < BB_BQUEUE_MAX_INSTANCES; i++) {
        if (s_pool[i].in_use) {
            bb_bqueue_destroy((bb_bqueue_t)&s_pool[i]);
        }
    }
    memset(s_pool, 0, sizeof(s_pool));
}

size_t bb_bqueue_test_waiting_count(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst) return 0;
    return atomic_load(&inst->waiting_count);
}

#endif /* BB_BQUEUE_TESTING */
