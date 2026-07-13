// bb_lock_cond — ESP-IDF backend: FreeRTOS-native condition variable.
//
// Composes with bb_lock_t via its PUBLIC bb_lock_lock()/bb_lock_unlock() API
// only — never the raw FreeRTOS mutex semaphore embedded inside it.
// bb_lock_t's ESP-IDF backend deliberately wraps xSemaphoreCreateMutex()
// directly rather than the ESP-IDF POSIX layer's pthread_mutex_t (see
// bb_lock.c's priority-inheritance rationale), so ESP-IDF's own
// pthread_cond_t — whose pthread_cond_wait()/pthread_cond_timedwait()
// require a pthread_mutex_t* to pair with — CANNOT be composed with bb_lock_t
// here. That type mismatch, not any doubt about ESP-IDF's own condvar
// algorithm, is why this file does not simply wrap <pthread.h>.
//
// Backend decision (B1-822): instead, this is a FreeRTOS-native
// implementation that deliberately mirrors the ALGORITHM ESP-IDF's own
// components/pthread/pthread_cond_var.c uses — a linked list of per-waiter
// counting semaphores, each waiter registered on the list BEFORE the
// caller's lock is released (closing the classic condvar lost-wakeup race:
// a signal/broadcast landing in the unlock-then-block window would otherwise
// never reach the waiter). signal() POPS (dequeues) the head waiter before
// giving to it; broadcast() pops every waiter in a loop, giving to each --
// never a give-without-dequeue, which would let a second back-to-back
// signal() land on the SAME already-full binary semaphore and silently drop
// that wakeup (mbedtls_rom_osi.c's pthread_cond_signal variant dequeues for
// the same reason). The list-splice algorithm itself is factored into the
// pure, host-testable bb_lock_cond_waiterlist.h/.c (no platform headers) --
// this file's own host-side test suite (test_bb_lock_cond_waiterlist.c)
// exercises push/pop/remove directly. This is a PROVEN design (upstream
// ESP-IDF ships it), not a novel one — chosen for honesty and footprint over
// both (a) re-deriving a bespoke FreeRTOS broadcast primitive from scratch
// (B1-821's bqueue does NOT consume this backend — it wraps a native
// FreeRTOS xQueue directly, so a subtly-wrong hand-rolled design here could
// go unnoticed for a long time), and (b) ESP-IDF's own pthread_cond_t, which
// is ruled out by the type mismatch above regardless.
//
// Each waiter's semaphore is a STACK-resident StaticSemaphore_t
// (xSemaphoreCreateBinaryStatic) — zero heap in the wait() hot path. Only
// the one-time per-condvar guard mutex (bb_lock_cond_init) is
// heap-allocated, matching bb_lock's own xSemaphoreCreateMutex() convention
// (see bb_lock.c) — not a per-operation cost.

#include "bb_lock.h"
#include "bb_lock_cond_waiterlist.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdatomic.h>

typedef struct bb_lock_cond_waiter {
    SemaphoreHandle_t sem;             // this waiter's own wake signal
    StaticSemaphore_t sem_buf;         // static storage backing sem — no heap
    bb_lock_cond_waiter_node_t node;   // list linkage (pure algorithm, see bb_lock_cond_waiterlist.h)
} bb_lock_cond_waiter_t;

typedef struct {
    SemaphoreHandle_t guard;                 // protects the waiter list below
    bb_lock_cond_waiter_node_t *waiters;     // singly-linked; nodes are stack-owned by waiting threads
} bb_lock_cond_impl_t;

_Static_assert(sizeof(bb_lock_cond_impl_t) <= BB_LOCK_COND_IMPL_STORAGE_BYTES,
               "bb_lock_cond_impl_t exceeds bb_lock_cond_t backend storage");

static inline bb_lock_cond_impl_t *bb_lock_cond_impl(bb_lock_cond_t *cond)
{
    return (bb_lock_cond_impl_t *)(void *)cond->bb_lock_cond_impl.bb_lock_cond_bytes;
}

static void bb_lock_cond_waiter_link(bb_lock_cond_impl_t *impl, bb_lock_cond_waiter_t *w)
{
    // portMAX_DELAY (no timeout) is safe here: `guard` is only ever held for
    // a short, bounded critical section (a list push/pop) and no user code
    // runs while it is held, so this cannot deadlock.
    xSemaphoreTake(impl->guard, portMAX_DELAY);
    bb_lock_cond_waiterlist_push(&impl->waiters, &w->node);
    xSemaphoreGive(impl->guard);
}

// Idempotent: a no-op if signal()/broadcast() already popped this waiter's
// node off the list (bb_lock_cond_waiterlist_remove() checks node->linked
// under this same guard) -- safe for wait()'s wake/timeout cleanup path to
// call unconditionally, whether the wake came from a real signal or a
// timeout race against one. Returns bb_lock_cond_waiterlist_remove()'s
// verdict: true if w was still linked (WE detached it, nobody signalled us),
// false if a signaller had already popped it (w WAS signalled) -- this is
// the AUTHORITATIVE signal observation used by wait() to decide its result,
// since the waiter's own xSemaphoreTake() return code races the pop (see
// bb_lock_cond_wait()). The guard is acquired unconditionally on every path
// here -- this, not the return value, is what prevents a use-after-free: it
// guarantees this waiter cannot leave scope while a signaller is mid-give to
// its (still stack-resident) semaphore.
static bool bb_lock_cond_waiter_unlink(bb_lock_cond_impl_t *impl, bb_lock_cond_waiter_t *w)
{
    // portMAX_DELAY (no timeout) is safe here for the same reason as
    // bb_lock_cond_waiter_link() above: `guard` is only ever held for a
    // short, bounded critical section and no user code runs while it is
    // held, so this cannot deadlock.
    xSemaphoreTake(impl->guard, portMAX_DELAY);
    bool was_still_linked = bb_lock_cond_waiterlist_remove(&impl->waiters, &w->node);
    xSemaphoreGive(impl->guard);
    return was_still_linked;
}

bb_err_t bb_lock_cond_init(bb_lock_cond_t *out)
{
    if (!out) {
        return BB_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    bb_lock_cond_impl_t *impl = bb_lock_cond_impl(out);
    impl->guard = xSemaphoreCreateMutex();
    if (!impl->guard) {
        return BB_ERR_NO_MEM;  // LCOV_EXCL_LINE — heap exhaustion at cond-create time is a real HIL scenario but not host-buildable/reproducible here
    }
    impl->waiters = NULL;
    atomic_store_explicit(&out->bb_lock_cond_initialized, true, memory_order_release);
    return BB_OK;
}

bb_err_t bb_lock_cond_destroy(bb_lock_cond_t *cond)
{
    if (!cond) {
        return BB_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&cond->bb_lock_cond_initialized, memory_order_acquire)) {
        // Never bb_lock_cond_init()'d — safe no-op.
        return BB_OK;
    }
    if (atomic_load_explicit(&cond->bb_lock_cond_destroyed, memory_order_acquire)) {
        // Double-destroy: never re-invoke vSemaphoreDelete on an
        // already-freed primitive.
        return BB_ERR_INVALID_STATE;
    }
    bb_lock_cond_impl_t *impl = bb_lock_cond_impl(cond);
    atomic_store_explicit(&cond->bb_lock_cond_destroyed, true, memory_order_release);
    if (impl->guard) {
        vSemaphoreDelete(impl->guard);
        impl->guard = NULL;
    }
    return BB_OK;
}

bb_err_t bb_lock_cond_wait(bb_lock_cond_t *cond, bb_lock_t *lock, uint32_t timeout_ms)
{
    if (!cond || !lock) {
        return BB_ERR_INVALID_ARG;
    }
    bb_lock_cond_impl_t *impl = bb_lock_cond_impl(cond);

    bb_lock_cond_waiter_t w;
    w.sem = xSemaphoreCreateBinaryStatic(&w.sem_buf);  // starts unavailable; first Take blocks
    w.node.wake = (void *)w.sem;
    w.node.next = NULL;
    w.node.linked = false;

    // Register BEFORE releasing the caller's lock -- closes the classic
    // condvar lost-wakeup race (see file header).
    bb_lock_cond_waiter_link(impl, &w);

    // Guaranteed to succeed: the caller held `lock` on entry (this
    // function's own precondition), so the backing primitive is known-live.
    bb_lock_unlock(lock);

    // NOT pdMS_TO_TICKS(timeout_ms) directly -- FreeRTOS computes that as
    // `(ms * configTICK_RATE_HZ) / 1000` in 32-bit TickType_t arithmetic,
    // which silently OVERFLOWS for a large-enough timeout_ms (a full
    // uint32_t range here) and produces a MUCH SHORTER wait than requested.
    // bb_lock_cond_ms_to_ticks() computes the same ratio in a 64-bit
    // intermediate and saturates instead of wrapping. max_ticks is clamped
    // to portMAX_DELAY - 1, never portMAX_DELAY itself, so a saturated
    // large-but-finite timeout can never be silently promoted into an
    // infinite wait.
    TickType_t ticks = (timeout_ms == BB_LOCK_COND_WAIT_FOREVER)
                            ? portMAX_DELAY
                            : (TickType_t)bb_lock_cond_ms_to_ticks(timeout_ms, configTICK_RATE_HZ, portMAX_DELAY - 1);
    BaseType_t got = xSemaphoreTake(w.sem, ticks);

    // Unregister unconditionally (signaled or timed out). Idempotent: if
    // signal()/broadcast() already popped this node off the list, this is a
    // no-op (see bb_lock_cond_waiter_unlink) -- either way, by the time this
    // returns nothing else holds a pointer into this about-to-leave-scope
    // waiter.
    //
    // was_still_linked is the AUTHORITATIVE signal observation -- NOT `got`.
    // Lost-wakeup regression (firmware review, B1-822 follow-up): xSemaphoreTake
    // timing out (got == pdFALSE) races signal()'s pop+give. If signal() pops
    // this waiter and gives to w.sem AFTER xSemaphoreTake has already given up
    // and returned pdFALSE, `got` alone would report a timeout even though the
    // waiter WAS popped -- and since it was popped, that signal can never reach
    // any other waiter either, so the wakeup is lost outright, not merely
    // misreported to this waiter. was_still_linked being false means exactly
    // "a signaller already popped me", which is signal delivery independent of
    // whatever `got` says -- see bb_lock_cond_waiterlist_decide_result().
    bool was_still_linked = bb_lock_cond_waiter_unlink(impl, &w);
    vSemaphoreDelete(w.sem);

    // Guaranteed to succeed: nothing else holds/destroys `lock` across this
    // wait (the caller owns it for the duration of the wait()/re-acquire
    // round-trip), so the backing primitive is known-live.
    bb_lock_lock(lock);

    return bb_lock_cond_waiterlist_decide_result(was_still_linked, got == pdTRUE);
}

bb_err_t bb_lock_cond_signal(bb_lock_cond_t *cond)
{
    if (!cond) {
        return BB_ERR_INVALID_ARG;
    }
    bb_lock_cond_impl_t *impl = bb_lock_cond_impl(cond);
    // portMAX_DELAY (no timeout) is safe here: `guard` is only ever held for
    // a short, bounded critical section and no user code runs while it is
    // held, so this cannot deadlock.
    xSemaphoreTake(impl->guard, portMAX_DELAY);
    // Pop (not peek) the head waiter BEFORE giving to it -- a second
    // back-to-back signal() must see the NEXT waiter as head, not give a
    // second time to the same (already-full) binary semaphore, which would
    // silently drop the wakeup. The give happens while still holding
    // `guard`, the same lock the popped waiter's own unlink() must acquire
    // before it can return from wait() and tear down its stack frame -- so
    // this can never race a use-after-free on w->wake.
    bb_lock_cond_waiter_node_t *w = bb_lock_cond_waiterlist_pop(&impl->waiters);
    if (w) {
        xSemaphoreGive((SemaphoreHandle_t)w->wake);
    }
    xSemaphoreGive(impl->guard);
    return BB_OK;
}

bb_err_t bb_lock_cond_broadcast(bb_lock_cond_t *cond)
{
    if (!cond) {
        return BB_ERR_INVALID_ARG;
    }
    bb_lock_cond_impl_t *impl = bb_lock_cond_impl(cond);
    // portMAX_DELAY (no timeout) is safe here: `guard` is only ever held for
    // a short, bounded critical section and no user code runs while it is
    // held, so this cannot deadlock.
    xSemaphoreTake(impl->guard, portMAX_DELAY);
    // Pop every waiter (detaching it) before giving to it -- same
    // no-double-give-to-a-detached-node contract as signal() above, applied
    // to the whole list.
    bb_lock_cond_waiter_node_t *w;
    while ((w = bb_lock_cond_waiterlist_pop(&impl->waiters)) != NULL) {
        xSemaphoreGive((SemaphoreHandle_t)w->wake);
    }
    xSemaphoreGive(impl->guard);
    return BB_OK;
}
