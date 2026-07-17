// Table-driven boundary tests for bb_age_classify() -- the pure age
// classifier shared by bb_cache's AGE_OUT eviction policy and bb_queue's
// age-eviction (B1-1031). NO locks, NO clock, NO I/O: mirrors
// test_bb_cache_evaluate.c's boundary cases against components/bb_core/src/
// bb_age.c in complete isolation.

#include "unity.h"
#include "bb_age.h"

#include <stdint.h>

typedef struct {
    const char     *name;
    uint64_t        age_ms;
    uint32_t        stale_age_ms;
    uint32_t        evict_age_ms;
    bb_age_state_t  want;
} age_case_t;

static const age_case_t s_cases[] = {
    // Ordinary stale window (stale_age_ms > 0).
    { "below_stale_is_fresh",       500,  1000, 5000, BB_AGE_FRESH },
    { "at_stale_boundary_is_stale", 1000, 1000, 5000, BB_AGE_STALE },
    { "between_stale_and_evict",    3000, 1000, 5000, BB_AGE_STALE },
    { "at_evict_boundary_is_evict", 5000, 1000, 5000, BB_AGE_EVICT },
    { "past_evict_is_evict",        9000, 1000, 5000, BB_AGE_EVICT },

    // stale_age_ms == 0 -- no stale window, FRESH until evict_age_ms.
    { "no_stale_window_zero_age_fresh",      0,    0, 5000, BB_AGE_FRESH },
    { "no_stale_window_mid_age_still_fresh", 4999, 0, 5000, BB_AGE_FRESH },
    { "no_stale_window_at_evict_boundary",   5000, 0, 5000, BB_AGE_EVICT },
    { "no_stale_window_past_evict",          9000, 0, 5000, BB_AGE_EVICT },

    // Zero age, non-zero stale window -- FRESH.
    { "zero_age_with_stale_window_fresh",    0, 1000, 5000, BB_AGE_FRESH },
};

void test_bb_age_classify_table(void)
{
    for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        const age_case_t *c = &s_cases[i];
        bb_age_state_t got = bb_age_classify(c->age_ms, c->stale_age_ms, c->evict_age_ms);
        TEST_ASSERT_EQUAL_MESSAGE(c->want, got, c->name);
    }
}

void test_bb_age_classify_evict_takes_priority_over_stale(void)
{
    // age_ms simultaneously satisfies both "age >= evict" and "age >= stale"
    // (evict_age_ms == stale_age_ms) -- EVICT must win.
    bb_age_state_t got = bb_age_classify(1000, 1000, 1000);
    TEST_ASSERT_EQUAL(BB_AGE_EVICT, got);
}
