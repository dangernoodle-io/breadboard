#pragma once
// bb_lock_cond_waiterlist — pure, platform-independent singly-linked waiter
// list algorithm shared by condvar backends that have no native OS condvar
// to lean on (currently: ESP-IDF's FreeRTOS-native bb_lock_cond backend, see
// platform/espidf/bb_core/bb_lock_cond.c). No platform headers, no heap --
// callers own node storage. Host-testable even though only the ESP-IDF
// backend links it (the host backend wraps pthread_cond_t directly and has
// no waiter list of its own).
//
// Contract: signal()-style consumers MUST use bb_lock_cond_waiterlist_pop(),
// never a peek-without-remove -- giving a wake to a node still linked in the
// list means a second signal() lands on the SAME node (a missed wakeup for
// every other waiter). broadcast()-style consumers pop every node in a loop.
// A waiter's own cleanup on wake/timeout MUST call
// bb_lock_cond_waiterlist_remove(), which is idempotent: a no-op if the node
// was already popped by signal()/broadcast() (checked via node->linked)
// under the SAME guard used to link/pop it -- callers are responsible for
// that external synchronization; this algorithm does no locking of its own.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "bb_core.h"

typedef struct bb_lock_cond_waiter_node {
    void *wake;                                // opaque per-waiter wake handle (e.g. a FreeRTOS SemaphoreHandle_t)
    bool linked;                                // true iff currently in the list; makes remove() idempotent
    struct bb_lock_cond_waiter_node *next;
} bb_lock_cond_waiter_node_t;

// Push w onto the head of *head. Sets w->linked = true.
void bb_lock_cond_waiterlist_push(bb_lock_cond_waiter_node_t **head, bb_lock_cond_waiter_node_t *w);

// Pop and return the head node (or NULL if the list is empty), detaching it
// from the list first (head advances, popped node's ->next is cleared and
// ->linked is set false). Two consecutive pops on a 2-node list return two
// DISTINCT nodes -- this is the property that fixes the missed-wakeup bug
// (a give-without-pop implementation would return the same node twice).
bb_lock_cond_waiter_node_t *bb_lock_cond_waiterlist_pop(bb_lock_cond_waiter_node_t **head);

// Remove w from *head if still linked; idempotent no-op if w->linked is
// already false (e.g. because signal()/pop() already detached it). Returns
// true if w was still linked when this call ran (i.e. WE detached it --
// nobody signalled w), false if it was already unlinked (i.e. a
// signaller had already popped it -- w WAS signalled). This return value is
// the AUTHORITATIVE signal-vs-timeout observation: a waiter's own wake
// primitive (e.g. a semaphore Take's return code) races the pop and cannot
// be trusted alone -- see bb_lock_cond_waiterlist_decide_result().
bool bb_lock_cond_waiterlist_remove(bb_lock_cond_waiter_node_t **head, bb_lock_cond_waiter_node_t *w);

// Pure decision helper: derive a waiter's wait() outcome from BOTH
// independent observations of "was I signalled" -- was_still_linked (from
// bb_lock_cond_waiterlist_remove()'s return) and sem_taken (the waiter's own
// wake-primitive result, e.g. xSemaphoreTake() == pdTRUE). was_still_linked
// is authoritative: if a signaller already popped this waiter off the list,
// it WAS signalled, and this returns BB_OK regardless of what the wake
// primitive itself reports (its result races the pop -- see
// platform/espidf/bb_core/bb_lock_cond.c's wait() for the exact
// interleaving this closes). sem_taken is honored defensively when the node
// was still linked (still_linked && sem_taken should never happen in
// practice -- a give only ever follows a pop -- but this never lets that
// combination produce a wrong answer).
bb_err_t bb_lock_cond_waiterlist_decide_result(bool was_still_linked, bool sem_taken);

// Pure, host-testable millisecond-to-ticks conversion for backends (e.g. the
// ESP-IDF bb_lock_cond_wait()) that must hand a finite timeout to a
// tick-based blocking primitive. timeout_ms is a full uint32_t range, but
// FreeRTOS's own pdMS_TO_TICKS() computes `(ms * tick_rate_hz) / 1000` in
// 32-bit TickType_t arithmetic, which silently OVERFLOWS/wraps for large ms
// (e.g. >~71 minutes at a 1000 Hz tick rate) and produces a MUCH SHORTER
// timeout than the caller asked for. This helper instead computes the same
// ratio in a 64-bit intermediate and SATURATES at max_ticks rather than
// wrapping -- a large-but-finite timeout_ms clamps to max_ticks, it never
// wraps around to a small value. Callers passing the BB_LOCK_COND_WAIT_FOREVER
// sentinel must special-case it themselves (this helper has no "forever"
// concept) and must choose max_ticks strictly less than their platform's own
// "block forever" sentinel (e.g. portMAX_DELAY - 1), so a saturated finite
// timeout can never be silently promoted into an infinite wait.
//
// Rounding: the ms->ticks ratio truncates (integer division), matching
// FreeRTOS's own pdMS_TO_TICKS() -- a sub-tick nonzero timeout_ms rounds DOWN
// to 0 ticks (e.g. timeout_ms=1 at tick_rate_hz=100 yields 0), which callers
// will treat as a non-blocking poll rather than a short wait. Intentional.
//
// Degenerate case: tick_rate_hz == 0 always yields 0 ticks (a non-blocking
// poll), regardless of timeout_ms, since the numerator is multiplied by 0.
// Harmless for the one real caller today (FreeRTOS's configTICK_RATE_HZ is
// always a positive Kconfig value) but a latent footgun for any future
// caller that passes a less-trusted tick_rate_hz -- callers MUST pass a real
// positive tick rate.
uint32_t bb_lock_cond_ms_to_ticks(uint32_t timeout_ms, uint32_t tick_rate_hz, uint32_t max_ticks);

// Pure, host-testable absolute-deadline computation for backends (e.g. the
// host bb_lock_cond_wait()'s Linux/glibc/musl CLOCK_MONOTONIC path, see
// platform/host/bb_core/bb_lock_cond.c) that must convert a "now" timestamp
// plus a RELATIVE timeout_ms into an ABSOLUTE struct timespec deadline for a
// timedwait-style primitive (e.g. pthread_cond_timedwait()). now.tv_nsec plus
// the fractional-millisecond-in-nanoseconds term can carry past 1e9 -- left
// unhandled, that hands an out-of-range timespec (tv_nsec >= 1000000000) to
// the timedwait call, which is undefined per POSIX. This helper takes `now`
// as a parameter rather than reading the clock itself specifically so BOTH
// the carry and no-carry paths are host-testable deterministically: whether
// the real clock_gettime() carries at any given instant depends on the
// current nanosecond-of-second, which is not something a test can control
// without this seam.
struct timespec bb_lock_cond_deadline_from_now(struct timespec now, uint32_t timeout_ms);
