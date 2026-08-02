#include "unity.h"
#include "../../components/bb_http_server/src/bb_http_core_steer.h"
#include "bb_wdt.h"

// bb_http_core_steer_resolve (B1-1364 PR5) -- thin wrapper over the already
// host-tested bb_wdt_steer_core(), adding exactly one piece of NEW decision
// logic: mapping BB_WDT_CORE_ANY (-1) to the caller-supplied "no affinity"
// sentinel (tskNO_AFFINITY on the real ESP-IDF shell; an arbitrary
// distinguishable value here). See bb_http_core_steer.h for the full
// rationale.

#define TEST_NO_AFFINITY ((int32_t)0x7FFFFFFF)

// ---------------------------------------------------------------------------
// Unconfigured (BB_WDT_CORE_ANY) + nothing claimed -- the default, unclaimed
// case. Must resolve to the "no affinity" sentinel unchanged, matching the
// pre-PR5 behavior of leaving HTTPD_DEFAULT_CONFIG()'s core_id (tskNO_AFFINITY)
// untouched.
// ---------------------------------------------------------------------------
void test_bb_http_core_steer_resolve_unconfigured_unclaimed_returns_no_affinity(void)
{
    TEST_ASSERT_EQUAL_INT32(TEST_NO_AFFINITY,
        bb_http_core_steer_resolve(BB_WDT_CORE_ANY, 0U, 2, TEST_NO_AFFINITY));
}

// ---------------------------------------------------------------------------
// Unconfigured + exactly one core claimed -- steers to the other core.
// ---------------------------------------------------------------------------
void test_bb_http_core_steer_resolve_steers_away_from_claimed_core0(void)
{
    TEST_ASSERT_EQUAL_INT32(1,
        bb_http_core_steer_resolve(BB_WDT_CORE_ANY, 1U << 0, 2, TEST_NO_AFFINITY));
}

void test_bb_http_core_steer_resolve_steers_away_from_claimed_core1(void)
{
    TEST_ASSERT_EQUAL_INT32(0,
        bb_http_core_steer_resolve(BB_WDT_CORE_ANY, 1U << 1, 2, TEST_NO_AFFINITY));
}

// ---------------------------------------------------------------------------
// Unconfigured + both cores claimed -- nothing to steer toward; falls back
// to "no affinity" (same as the unclaimed case).
// ---------------------------------------------------------------------------
void test_bb_http_core_steer_resolve_both_claimed_returns_no_affinity(void)
{
    TEST_ASSERT_EQUAL_INT32(TEST_NO_AFFINITY,
        bb_http_core_steer_resolve(BB_WDT_CORE_ANY, (1U << 0) | (1U << 1), 2,
                                    TEST_NO_AFFINITY));
}

// ---------------------------------------------------------------------------
// Explicit pin always wins over steering, even onto a claimed core.
// ---------------------------------------------------------------------------
void test_bb_http_core_steer_resolve_explicit_pin_passes_through_even_when_claimed(void)
{
    TEST_ASSERT_EQUAL_INT32(0,
        bb_http_core_steer_resolve(0, 1U << 0, 2, TEST_NO_AFFINITY));
}

// ---------------------------------------------------------------------------
// Unicore target (num_cores < 2): always returns the configured request
// unchanged -- there is nothing to steer away from. Covers the esp32c3/
// esp32s2 unicore path.
// ---------------------------------------------------------------------------
void test_bb_http_core_steer_resolve_unicore_unconfigured_returns_no_affinity(void)
{
    TEST_ASSERT_EQUAL_INT32(TEST_NO_AFFINITY,
        bb_http_core_steer_resolve(BB_WDT_CORE_ANY, 1U << 0, 1, TEST_NO_AFFINITY));
}

void test_bb_http_core_steer_resolve_unicore_explicit_pin_unchanged(void)
{
    TEST_ASSERT_EQUAL_INT32(0,
        bb_http_core_steer_resolve(0, 1U << 0, 1, TEST_NO_AFFINITY));
}
