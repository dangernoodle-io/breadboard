// Table-driven boundary tests for bb_cache_evaluate_age() -- the pure
// classifier behind bb_cache's opt-in AGE_OUT eviction policy (B1-592 A3).
// NO locks, NO clock, NO bb_cache registry involved: this file exercises
// components/bb_cache/src/bb_cache_evaluate.c in complete isolation.
//
// Register-time rejection of stale_age_ms >= evict_age_ms is a
// bb_cache_register() GUARD test, not an evaluate test -- see
// test_bb_cache_evict.c's register-guard tests instead.

#include "unity.h"
#include "bb_cache.h"

#include <stdint.h>

typedef struct {
    const char             *name;
    uint64_t                age_ms;
    uint32_t                stale_age_ms;
    uint32_t                evict_age_ms;
    bb_cache_entry_state_t  want;
} evaluate_case_t;

static const evaluate_case_t s_cases[] = {
    // Ordinary stale window (stale_age_ms > 0).
    { "below_stale_is_fresh",       500,  1000, 5000, BB_CACHE_ENTRY_FRESH },
    { "at_stale_boundary_is_stale", 1000, 1000, 5000, BB_CACHE_ENTRY_STALE },
    { "between_stale_and_evict",    3000, 1000, 5000, BB_CACHE_ENTRY_STALE },
    { "at_evict_boundary_is_evict", 5000, 1000, 5000, BB_CACHE_ENTRY_EVICT },
    { "past_evict_is_evict",        9000, 1000, 5000, BB_CACHE_ENTRY_EVICT },

    // stale_age_ms == 0 -- no stale window, FRESH until evict_age_ms.
    { "no_stale_window_zero_age_fresh",        0,    0, 5000, BB_CACHE_ENTRY_FRESH },
    { "no_stale_window_mid_age_still_fresh",   4999, 0, 5000, BB_CACHE_ENTRY_FRESH },
    { "no_stale_window_at_evict_boundary",     5000, 0, 5000, BB_CACHE_ENTRY_EVICT },
    { "no_stale_window_past_evict",            9000, 0, 5000, BB_CACHE_ENTRY_EVICT },

    // Zero age, non-zero stale window -- FRESH.
    { "zero_age_with_stale_window_fresh",      0, 1000, 5000, BB_CACHE_ENTRY_FRESH },
};

void test_bb_cache_evaluate_age_table(void)
{
    for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        const evaluate_case_t *c = &s_cases[i];
        bb_cache_entry_state_t got =
            bb_cache_evaluate_age(c->age_ms, c->stale_age_ms, c->evict_age_ms);
        TEST_ASSERT_EQUAL_MESSAGE(c->want, got, c->name);
    }
}

void test_bb_cache_evaluate_age_evict_takes_priority_over_stale(void)
{
    // age_ms simultaneously satisfies both "age >= evict" and would satisfy
    // "age >= stale" -- EVICT must win (evict_age_ms == stale_age_ms edge
    // case, which bb_cache_register() itself rejects at register time, but
    // the pure classifier must still resolve deterministically for any
    // input it is given).
    bb_cache_entry_state_t got = bb_cache_evaluate_age(1000, 1000, 1000);
    TEST_ASSERT_EQUAL(BB_CACHE_ENTRY_EVICT, got);
}
