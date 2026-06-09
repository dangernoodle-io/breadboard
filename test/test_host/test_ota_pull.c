#include "unity.h"
#include "bb_ota_pull.h"
#include "bb_ota_pull_test_hooks.h"
#include <stdint.h>

void test_bb_ota_pull_set_http_timeout_ms_default_is_20000(void)
{
    // Default must be 20000 ms for backward compat with existing consumers.
    // Restore default via 0 before asserting to avoid cross-test pollution.
    bb_ota_pull_set_http_timeout_ms(0);
    TEST_ASSERT_EQUAL_UINT32(20000, bb_ota_pull_host_get_http_timeout_ms());
}

void test_bb_ota_pull_set_http_timeout_ms_zero_restores_default(void)
{
    // Set a non-default value, then pass 0 — must restore 20000.
    bb_ota_pull_set_http_timeout_ms(60000);
    TEST_ASSERT_EQUAL_UINT32(60000, bb_ota_pull_host_get_http_timeout_ms());
    bb_ota_pull_set_http_timeout_ms(0);
    TEST_ASSERT_EQUAL_UINT32(20000, bb_ota_pull_host_get_http_timeout_ms());
}

// WDT exclusion (ota_worker_task / ota_check_worker_task) is ESP-IDF-only;
// it requires esp_task_wdt_delete/add which have no host stub. Correctness
// is verified by code review and smoke builds against espidf targets.

// ---------------------------------------------------------------------------
// bb_ota_pull_apply_cache_is_fresh — pure freshness predicate
// ---------------------------------------------------------------------------

// Helper: 5-minute window in microseconds.
#define WINDOW_5MIN_S 300
#define ONE_MIN_US    (60LL * 1000000LL)
#define FIVE_MIN_US   (300LL * 1000000LL)

void test_apply_cache_is_fresh_fresh_result_returns_true(void)
{
    // last_check 1 minute ago, 5-minute window -> fresh
    int64_t now_us  = 10000000000LL;
    int64_t last_us = now_us - ONE_MIN_US;
    TEST_ASSERT_TRUE(bb_ota_pull_apply_cache_is_fresh(true, last_us, now_us, WINDOW_5MIN_S));
}

void test_apply_cache_is_fresh_stale_result_returns_false(void)
{
    // last_check 10 minutes ago, 5-minute window -> stale
    int64_t now_us  = 10000000000LL;
    int64_t last_us = now_us - (10LL * ONE_MIN_US);
    TEST_ASSERT_FALSE(bb_ota_pull_apply_cache_is_fresh(true, last_us, now_us, WINDOW_5MIN_S));
}

void test_apply_cache_is_fresh_never_checked_returns_false(void)
{
    // last_check_us == 0 -> never checked
    int64_t now_us = 10000000000LL;
    TEST_ASSERT_FALSE(bb_ota_pull_apply_cache_is_fresh(false, 0, now_us, WINDOW_5MIN_S));
}

void test_apply_cache_is_fresh_last_check_ok_false_returns_false(void)
{
    // last_check_ok=false -> stale regardless of age
    int64_t now_us  = 10000000000LL;
    int64_t last_us = now_us - ONE_MIN_US;  // recent, but failed
    TEST_ASSERT_FALSE(bb_ota_pull_apply_cache_is_fresh(false, last_us, now_us, WINDOW_5MIN_S));
}

void test_apply_cache_is_fresh_window_zero_always_returns_false(void)
{
    // window_s == 0 -> always refresh regardless of cache age
    int64_t now_us  = 10000000000LL;
    int64_t last_us = now_us - 1000LL;  // 1 ms ago - very recent
    TEST_ASSERT_FALSE(bb_ota_pull_apply_cache_is_fresh(true, last_us, now_us, 0));
}

void test_apply_cache_is_fresh_exactly_at_window_boundary_returns_true(void)
{
    // age == window (exactly at the boundary) -> fresh (<=)
    int64_t now_us  = 10000000000LL;
    int64_t last_us = now_us - FIVE_MIN_US;
    TEST_ASSERT_TRUE(bb_ota_pull_apply_cache_is_fresh(true, last_us, now_us, WINDOW_5MIN_S));
}

void test_apply_cache_is_fresh_one_us_past_window_returns_false(void)
{
    // age == window + 1 us -> stale
    int64_t now_us  = 10000000000LL;
    int64_t last_us = now_us - FIVE_MIN_US - 1LL;
    TEST_ASSERT_FALSE(bb_ota_pull_apply_cache_is_fresh(true, last_us, now_us, WINDOW_5MIN_S));
}

// ---------------------------------------------------------------------------
// bb_ota_pull_download_should_retry — pure retry-decision helper
// ---------------------------------------------------------------------------

void test_download_should_retry_ok_and_complete_is_false(void)
{
    // perform returned OK and data is complete — no retry needed
    TEST_ASSERT_FALSE(bb_ota_pull_download_should_retry(0, true));
}

void test_download_should_retry_perform_error_triggers_retry(void)
{
    // perform exited with an error — retry regardless of complete flag
    TEST_ASSERT_TRUE(bb_ota_pull_download_should_retry(1, true));
    TEST_ASSERT_TRUE(bb_ota_pull_download_should_retry(1, false));
}

void test_download_should_retry_incomplete_data_triggers_retry(void)
{
    // perform returned OK but data is incomplete (e.g. CDN drop at 54%) — retry
    TEST_ASSERT_TRUE(bb_ota_pull_download_should_retry(0, false));
}

void test_download_should_retry_both_error_and_incomplete_triggers_retry(void)
{
    // both conditions true — still retry
    TEST_ASSERT_TRUE(bb_ota_pull_download_should_retry(-1, false));
}
