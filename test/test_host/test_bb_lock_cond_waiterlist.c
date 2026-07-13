// Host tests for the pure bb_lock_cond_waiterlist algorithm (B1-822 review
// MED-1): this is the exact list-splice logic bb_lock_cond.c's ESP-IDF
// backend uses for signal()/broadcast() -- no FreeRTOS involved, so it is
// directly host-testable even though only the ESP-IDF backend links it.
//
// The critical property under test: signal() (mirrored here as one
// bb_lock_cond_waiterlist_pop() call) must POP (dequeue) the head waiter,
// not merely peek it -- otherwise two back-to-back signals both target the
// same still-linked node, and the SECOND signal's wakeup is silently lost
// (whatever backing wake primitive the caller gives to a second time just
// silently no-ops on an already-signaled binary semaphore).
#include "unity.h"
#include "bb_lock_cond_waiterlist.h"
#include <stdint.h>
#include <string.h>

// setUp/tearDown: defined in test_main.c (global).

static bb_lock_cond_waiter_node_t bb_lock_cond_waiterlist_test_mk(int wake_id)
{
    bb_lock_cond_waiter_node_t n;
    memset(&n, 0, sizeof(n));
    n.wake = (void *)(intptr_t)wake_id;
    return n;
}

// ---------------------------------------------------------------------------
// push / pop basics
// ---------------------------------------------------------------------------

void test_bb_lock_cond_waiterlist_pop_empty_list_returns_null(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    TEST_ASSERT_NULL(bb_lock_cond_waiterlist_pop(&head));
}

void test_bb_lock_cond_waiterlist_push_sets_linked_true(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    bb_lock_cond_waiter_node_t w = bb_lock_cond_waiterlist_test_mk(1);
    TEST_ASSERT_FALSE(w.linked);
    bb_lock_cond_waiterlist_push(&head, &w);
    TEST_ASSERT_TRUE(w.linked);
    TEST_ASSERT_EQUAL_PTR(&w, head);
}

void test_bb_lock_cond_waiterlist_pop_returns_most_recently_pushed(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    bb_lock_cond_waiter_node_t a = bb_lock_cond_waiterlist_test_mk(1);
    bb_lock_cond_waiter_node_t b = bb_lock_cond_waiterlist_test_mk(2);
    bb_lock_cond_waiterlist_push(&head, &a);
    bb_lock_cond_waiterlist_push(&head, &b);

    bb_lock_cond_waiter_node_t *popped = bb_lock_cond_waiterlist_pop(&head);
    TEST_ASSERT_EQUAL_PTR(&b, popped);
    TEST_ASSERT_FALSE(popped->linked);
    TEST_ASSERT_NULL(popped->next);
}

// ---------------------------------------------------------------------------
// MED-1 -- the core missed-wakeup fix: two consecutive pops on a 2-waiter
// list return two DISTINCT nodes, never the same node twice.
//
// Non-vacuity proof (per review): this test was run against a deliberate
// revert to the OLD buggy behavior (peek-the-head-without-dequeuing, i.e.
// bb_lock_cond_waiterlist_pop() rewritten to just `return *head;` with no
// mutation) -- observed FAILING: both pop1 and pop2 returned the SAME node
// (&b), so the `TEST_ASSERT_NOT_EQUAL` below tripped and the "two distinct
// wake targets" assertion failed. Restoring the real pop-and-dequeue
// implementation makes it pass. This is exactly the bug: a second
// back-to-back signal() landing on an already-signaled waiter's semaphore
// would silently drop that wakeup.
// ---------------------------------------------------------------------------

void test_bb_lock_cond_waiterlist_two_pops_after_two_pushes_return_distinct_nodes(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    bb_lock_cond_waiter_node_t a = bb_lock_cond_waiterlist_test_mk(1);
    bb_lock_cond_waiter_node_t b = bb_lock_cond_waiterlist_test_mk(2);
    bb_lock_cond_waiterlist_push(&head, &a);
    bb_lock_cond_waiterlist_push(&head, &b);

    bb_lock_cond_waiter_node_t *pop1 = bb_lock_cond_waiterlist_pop(&head);
    bb_lock_cond_waiter_node_t *pop2 = bb_lock_cond_waiterlist_pop(&head);

    TEST_ASSERT_NOT_NULL(pop1);
    TEST_ASSERT_NOT_NULL(pop2);
    TEST_ASSERT_TRUE(pop1 != pop2);           // two DISTINCT waiters woken
    TEST_ASSERT_NULL(bb_lock_cond_waiterlist_pop(&head)); // list now empty
}

// Mirrors "signal wakes exactly one": a single pop detaches only the head
// waiter -- the other waiter remains linked (i.e. would still be blocked).
void test_bb_lock_cond_waiterlist_one_pop_leaves_other_waiter_linked(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    bb_lock_cond_waiter_node_t a = bb_lock_cond_waiterlist_test_mk(1);
    bb_lock_cond_waiter_node_t b = bb_lock_cond_waiterlist_test_mk(2);
    bb_lock_cond_waiterlist_push(&head, &a);
    bb_lock_cond_waiterlist_push(&head, &b);

    bb_lock_cond_waiter_node_t *popped = bb_lock_cond_waiterlist_pop(&head);
    TEST_ASSERT_EQUAL_PTR(&b, popped);
    TEST_ASSERT_TRUE(a.linked);
    TEST_ASSERT_EQUAL_PTR(&a, head);
}

// ---------------------------------------------------------------------------
// remove() -- broadcast()'s per-node detach, and wait()'s own cleanup path.
// ---------------------------------------------------------------------------

void test_bb_lock_cond_waiterlist_remove_detaches_middle_node(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    bb_lock_cond_waiter_node_t a = bb_lock_cond_waiterlist_test_mk(1);
    bb_lock_cond_waiter_node_t b = bb_lock_cond_waiterlist_test_mk(2);
    bb_lock_cond_waiter_node_t c = bb_lock_cond_waiterlist_test_mk(3);
    bb_lock_cond_waiterlist_push(&head, &a); // head: a
    bb_lock_cond_waiterlist_push(&head, &b); // head: b -> a
    bb_lock_cond_waiterlist_push(&head, &c); // head: c -> b -> a

    TEST_ASSERT_TRUE(bb_lock_cond_waiterlist_remove(&head, &b));

    TEST_ASSERT_FALSE(b.linked);
    TEST_ASSERT_NULL(b.next);
    TEST_ASSERT_EQUAL_PTR(&c, head);
    TEST_ASSERT_EQUAL_PTR(&a, c.next);
}

// The idempotency property that makes wait()'s unconditional cleanup call
// safe: removing an already-popped (or already-removed) node is a no-op,
// never touching the list a second time.
void test_bb_lock_cond_waiterlist_remove_after_pop_is_idempotent_noop(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    bb_lock_cond_waiter_node_t a = bb_lock_cond_waiterlist_test_mk(1);
    bb_lock_cond_waiterlist_push(&head, &a);

    bb_lock_cond_waiter_node_t *popped = bb_lock_cond_waiterlist_pop(&head);
    TEST_ASSERT_EQUAL_PTR(&a, popped);
    TEST_ASSERT_NULL(head);

    // Simulates wait()'s own cleanup path running after signal() already
    // popped this node -- must be a safe no-op, not a double-unlink. The
    // false return is the "I WAS signalled" observation wait() relies on.
    TEST_ASSERT_FALSE(bb_lock_cond_waiterlist_remove(&head, &a));
    TEST_ASSERT_FALSE(a.linked);
    TEST_ASSERT_NULL(head);
}

void test_bb_lock_cond_waiterlist_remove_twice_is_idempotent_noop(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    bb_lock_cond_waiter_node_t a = bb_lock_cond_waiterlist_test_mk(1);
    bb_lock_cond_waiterlist_push(&head, &a);

    TEST_ASSERT_TRUE(bb_lock_cond_waiterlist_remove(&head, &a));
    TEST_ASSERT_NULL(head);

    // Second remove on the same node: still a no-op, list stays empty, and
    // now reports false (already detached).
    TEST_ASSERT_FALSE(bb_lock_cond_waiterlist_remove(&head, &a));
    TEST_ASSERT_FALSE(a.linked);
    TEST_ASSERT_NULL(head);
}

void test_bb_lock_cond_waiterlist_remove_never_linked_is_noop(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    bb_lock_cond_waiter_node_t a = bb_lock_cond_waiterlist_test_mk(1);
    // a was never pushed -- linked defaults false via the test helper's memset.
    TEST_ASSERT_FALSE(bb_lock_cond_waiterlist_remove(&head, &a));
    TEST_ASSERT_FALSE(a.linked);
    TEST_ASSERT_NULL(head);
}

// Defensive branch: a node claiming linked==true that is not actually
// present in *head (an invariant violation callers should never produce,
// since push()/pop() keep ->linked and real list membership in sync) still
// exhausts the walk safely -- the loop reaches an empty tail without
// matching, and remove() just clears the caller's now-inconsistent flags
// rather than corrupting someone else's list.
void test_bb_lock_cond_waiterlist_remove_linked_but_not_present_walks_to_end_safely(void)
{
    bb_lock_cond_waiter_node_t *head = NULL;
    bb_lock_cond_waiter_node_t a = bb_lock_cond_waiterlist_test_mk(1);
    bb_lock_cond_waiterlist_push(&head, &a); // list: a

    bb_lock_cond_waiter_node_t stray = bb_lock_cond_waiterlist_test_mk(2);
    stray.linked = true; // simulate the invariant violation directly

    TEST_ASSERT_TRUE(bb_lock_cond_waiterlist_remove(&head, &stray));

    TEST_ASSERT_FALSE(stray.linked);
    TEST_ASSERT_NULL(stray.next);
    // The real list is untouched.
    TEST_ASSERT_EQUAL_PTR(&a, head);
    TEST_ASSERT_TRUE(a.linked);
}

// ---------------------------------------------------------------------------
// decide_result() -- the pure result-decision helper that fixes the HIGH
// lost-wakeup regression: `was_still_linked` (from remove()'s return) is the
// AUTHORITATIVE signal observation, not the waiter's own wake-primitive
// result (sem_taken), because that result races the pop performed by
// signal()/broadcast() -- see platform/espidf/bb_core/bb_lock_cond.c's
// wait().
//
// Non-vacuity proof (per review): this test was run against a deliberate
// revert to the OLD buggy decision logic --
// `return sem_taken ? BB_OK : BB_ERR_TIMEOUT;` (ignoring was_still_linked
// entirely, mirroring the original `(got == pdTRUE) ? BB_OK :
// BB_ERR_TIMEOUT` bug) -- observed FAILING on exactly the race case:
// was_still_linked=false, sem_taken=false returned BB_ERR_TIMEOUT instead of
// the required BB_OK, tripping the assertion below. Restoring the real
// was_still_linked-is-authoritative implementation makes it pass. This IS
// the bug: a waiter popped by signal() but whose own xSemaphoreTake() lost
// the race and returned pdFALSE would wrongly report BB_ERR_TIMEOUT even
// though the signal was consumed and can never reach any other waiter.
// ---------------------------------------------------------------------------

void test_bb_lock_cond_waiterlist_decide_result_still_linked_and_not_taken_is_timeout(void)
{
    // Genuine timeout: nobody popped us, and our own take didn't succeed.
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_lock_cond_waiterlist_decide_result(true, false));
}

void test_bb_lock_cond_waiterlist_decide_result_popped_but_take_lost_the_race_is_ok(void)
{
    // THE BUG CASE: a signaller popped us (was_still_linked=false) but our
    // own xSemaphoreTake() still reports it didn't succeed (sem_taken=false)
    // because the timeout won the race against the give. We WERE signalled
    // -- this MUST be BB_OK, never BB_ERR_TIMEOUT, or the signal (and the
    // producer's item behind it) is silently lost.
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_waiterlist_decide_result(false, false));
}

void test_bb_lock_cond_waiterlist_decide_result_not_linked_and_taken_is_ok(void)
{
    // The expected/common signalled path: popped AND the take succeeded.
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_waiterlist_decide_result(false, true));
}

void test_bb_lock_cond_waiterlist_decide_result_still_linked_and_taken_is_ok(void)
{
    // Defensive combination that should be unreachable in practice (a give
    // only ever follows a pop) -- must not steer toward a wrong answer.
    TEST_ASSERT_EQUAL(BB_OK, bb_lock_cond_waiterlist_decide_result(true, true));
}

// ---------------------------------------------------------------------------
// bb_lock_cond_ms_to_ticks() -- MEDIUM review finding: FreeRTOS's own
// pdMS_TO_TICKS() computes `(ms * tick_rate_hz) / 1000` in 32-bit TickType_t
// arithmetic, which silently OVERFLOWS/wraps for a large-enough timeout_ms
// and produces a MUCH SHORTER wait than requested. This helper computes the
// same ratio in a 64-bit intermediate and SATURATES at max_ticks instead of
// wrapping.
//
// Non-vacuity proof (per review): the overflow-regression test below
// (test_bb_lock_cond_ms_to_ticks_large_value_that_would_overflow_32_bit_math_does_not_wrap)
// was run against a deliberate reintroduction of 32-bit-only math --
// `return (uint32_t)(((uint32_t)timeout_ms * (uint32_t)tick_rate_hz) / 1000u);`
// (dropping the uint64_t intermediate and the saturation clamp entirely) --
// observed FAILING: 4294968 * 1000 wraps to 704 in 32-bit arithmetic, so
// 704 / 1000 == 0 was returned instead of the correct 4294968, tripping the
// TEST_ASSERT_EQUAL_UINT32 below. Restoring the real 64-bit-intermediate,
// saturating implementation makes it pass. This IS the bug: a caller
// requesting a ~71-minute-plus timeout would silently get an ~0-tick (i.e.
// effectively immediate) timeout instead.
// ---------------------------------------------------------------------------

void test_bb_lock_cond_ms_to_ticks_zero_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, bb_lock_cond_ms_to_ticks(0u, 1000u, UINT32_MAX - 1u));
}

void test_bb_lock_cond_ms_to_ticks_small_value_below_max(void)
{
    TEST_ASSERT_EQUAL_UINT32(500u, bb_lock_cond_ms_to_ticks(500u, 1000u, UINT32_MAX - 1u));
}

void test_bb_lock_cond_ms_to_ticks_exact_boundary_equals_max_is_not_saturated(void)
{
    // True ratio lands EXACTLY on max_ticks -- must return max_ticks itself,
    // not treat "== max_ticks" as an overflow case.
    TEST_ASSERT_EQUAL_UINT32(1000u, bb_lock_cond_ms_to_ticks(1000u, 1000u, 1000u));
}

void test_bb_lock_cond_ms_to_ticks_large_value_that_would_overflow_32_bit_math_does_not_wrap(void)
{
    // ms * tick_rate_hz = 4294968 * 1000 = 4294968000, which exceeds
    // UINT32_MAX (4294967295) -- a 32-bit-only multiply-then-divide (the
    // pdMS_TO_TICKS() bug this helper fixes) wraps that product to 704
    // before dividing, yielding 0 ticks. The true (correct) ratio is
    // 4294968, well under this test's generous max_ticks ceiling, so the
    // correct answer is NOT a saturation case either -- it must come out
    // exactly right.
    TEST_ASSERT_EQUAL_UINT32(4294968u, bb_lock_cond_ms_to_ticks(4294968u, 1000u, UINT32_MAX - 1u));
}

void test_bb_lock_cond_ms_to_ticks_saturates_at_max_ticks_ceiling(void)
{
    // True ratio (5000) exceeds max_ticks (1000) -- must clamp to max_ticks,
    // never wrap and never exceed the ceiling.
    TEST_ASSERT_EQUAL_UINT32(1000u, bb_lock_cond_ms_to_ticks(5000u, 1000u, 1000u));
}

void test_bb_lock_cond_ms_to_ticks_sub_tick_nonzero_ms_rounds_down_to_zero(void)
{
    // A NONZERO timeout_ms that is still less than one tick's worth of ms
    // truncates to 0 ticks -- matches FreeRTOS's own pdMS_TO_TICKS()
    // rounding, but is distinct from (and previously untested alongside) the
    // timeout_ms == 0 case above: 1 * 100 / 1000 == 0.
    TEST_ASSERT_EQUAL_UINT32(0u, bb_lock_cond_ms_to_ticks(1u, 100u, UINT32_MAX - 1u));
}

// ---------------------------------------------------------------------------
// bb_lock_cond_deadline_from_now() -- the host bb_lock_cond_wait()'s
// Linux/glibc/musl absolute-deadline computation (platform/host/bb_core/
// bb_lock_cond.c), extracted to a pure function so both the tv_nsec-carry and
// no-carry paths are deterministically host-testable. Whether the REAL
// clock_gettime() carries at any given instant is a function of the current
// nanosecond-of-second and is not otherwise controllable from a test.
// ---------------------------------------------------------------------------

void test_bb_lock_cond_deadline_from_now_no_carry_adds_seconds_and_nanos(void)
{
    struct timespec now = { .tv_sec = 100, .tv_nsec = 200000000L }; // 100.2s
    struct timespec deadline = bb_lock_cond_deadline_from_now(now, 1500u); // +1.5s

    // 100.2 + 1.5 = 101.7s -- no carry (tv_nsec stays under 1e9).
    TEST_ASSERT_EQUAL_INT64(101, (int64_t)deadline.tv_sec);
    TEST_ASSERT_EQUAL_INT64(700000000L, (int64_t)deadline.tv_nsec);
}

void test_bb_lock_cond_deadline_from_now_carry_rolls_into_next_second(void)
{
    struct timespec now = { .tv_sec = 100, .tv_nsec = 900000000L }; // 100.9s
    struct timespec deadline = bb_lock_cond_deadline_from_now(now, 300u); // +0.3s

    // 100.9 + 0.3 = 101.2s -- tv_nsec sum (900ms + 300ms = 1200ms) carries
    // past 1e9, so this MUST land on tv_sec=101, tv_nsec=200000000, never the
    // out-of-range tv_sec=100, tv_nsec=1200000000 a naive add would produce.
    TEST_ASSERT_EQUAL_INT64(101, (int64_t)deadline.tv_sec);
    TEST_ASSERT_EQUAL_INT64(200000000L, (int64_t)deadline.tv_nsec);
}

void test_bb_lock_cond_deadline_from_now_carry_exact_boundary_is_not_carried(void)
{
    struct timespec now = { .tv_sec = 5, .tv_nsec = 500000000L };
    struct timespec deadline = bb_lock_cond_deadline_from_now(now, 500u); // +0.5s

    // Sum lands EXACTLY at 1e9 minus one tick below the carry threshold used
    // here (500ms + 500ms = 1000ms == 1e9ns) -- the >= comparison in the
    // implementation means this IS a carry case: must roll to tv_sec=6,
    // tv_nsec=0, not tv_sec=5, tv_nsec=1000000000 (out of range).
    TEST_ASSERT_EQUAL_INT64(6, (int64_t)deadline.tv_sec);
    TEST_ASSERT_EQUAL_INT64(0L, (int64_t)deadline.tv_nsec);
}

void test_bb_lock_cond_deadline_from_now_zero_timeout_returns_now(void)
{
    struct timespec now = { .tv_sec = 42, .tv_nsec = 123456789L };
    struct timespec deadline = bb_lock_cond_deadline_from_now(now, 0u);

    TEST_ASSERT_EQUAL_INT64(42, (int64_t)deadline.tv_sec);
    TEST_ASSERT_EQUAL_INT64(123456789L, (int64_t)deadline.tv_nsec);
}
