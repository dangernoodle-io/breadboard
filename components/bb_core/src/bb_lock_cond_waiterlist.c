// Pure (host-testable) waiter-list algorithm for bb_lock_cond's ESP-IDF
// backend -- see bb_lock_cond_waiterlist.h for the contract. No platform
// headers here -- compiled on host and ESP-IDF identically.
#include "bb_lock_cond_waiterlist.h"

void bb_lock_cond_waiterlist_push(bb_lock_cond_waiter_node_t **head, bb_lock_cond_waiter_node_t *w)
{
    w->next = *head;
    w->linked = true;
    *head = w;
}

bb_lock_cond_waiter_node_t *bb_lock_cond_waiterlist_pop(bb_lock_cond_waiter_node_t **head)
{
    bb_lock_cond_waiter_node_t *w = *head;
    if (!w) {
        return NULL;
    }
    *head = w->next;
    w->next = NULL;
    w->linked = false;
    return w;
}

bool bb_lock_cond_waiterlist_remove(bb_lock_cond_waiter_node_t **head, bb_lock_cond_waiter_node_t *w)
{
    if (!w->linked) {
        // Already detached by signal()/pop() (or never linked) -- no-op.
        // Idempotency is what makes it safe for a waiter's own wake/timeout
        // cleanup path to call this unconditionally. w was already popped by
        // a signaller, so it WAS signalled -- report false.
        return false;
    }
    bb_lock_cond_waiter_node_t **p = head;
    while (*p) {
        if (*p == w) {
            *p = w->next;
            break;
        }
        p = &(*p)->next;
    }
    w->next = NULL;
    w->linked = false;
    return true;
}

bb_err_t bb_lock_cond_waiterlist_decide_result(bool was_still_linked, bool sem_taken)
{
    // was_still_linked is authoritative: a signaller popping this waiter off
    // the list IS the signal, independent of whatever the wake primitive's
    // own return code says (that result races the pop -- see
    // platform/espidf/bb_core/bb_lock_cond.c's wait()). sem_taken is honored
    // defensively for the still_taken case, which should be unreachable in
    // practice (a give only ever follows a pop) but must never steer this
    // toward a wrong answer.
    return (!was_still_linked || sem_taken) ? BB_OK : BB_ERR_TIMEOUT;
}

uint32_t bb_lock_cond_ms_to_ticks(uint32_t timeout_ms, uint32_t tick_rate_hz, uint32_t max_ticks)
{
    // 64-bit intermediate -- the whole point is to never let this product
    // wrap in 32-bit arithmetic the way pdMS_TO_TICKS() does.
    uint64_t ticks = ((uint64_t)timeout_ms * (uint64_t)tick_rate_hz) / 1000u;
    if (ticks > (uint64_t)max_ticks) {
        return max_ticks;  // saturate, never wrap
    }
    return (uint32_t)ticks;
}

struct timespec bb_lock_cond_deadline_from_now(struct timespec now, uint32_t timeout_ms)
{
    struct timespec deadline = now;
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }
    return deadline;
}
