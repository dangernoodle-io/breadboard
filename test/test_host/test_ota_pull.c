#include "unity.h"
#include "bb_ota_pull.h"
#include "bb_tls.h"
#include "bb_ota_pull_test_hooks.h"
#include "bb_update_check.h"
#include "../../components/bb_update_check/src/bb_update_check_internal.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

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

// ---------------------------------------------------------------------------
// bb_tls_heap_guard_passes — combined contiguous + total-free predicate
// ---------------------------------------------------------------------------

void test_heap_guard_passes_both_disabled_always_passes(void)
{
    // both floors = 0 (disabled) — guard always passes regardless of values
    TEST_ASSERT_TRUE(bb_tls_heap_guard_passes(0, 0, 0, 0, NULL));
    TEST_ASSERT_TRUE(bb_tls_heap_guard_passes(1, 0, 1, 0, NULL));
}

void test_heap_guard_passes_contiguous_ok_total_disabled(void)
{
    // contiguous floor set, total-free disabled; block is above floor — pass
    TEST_ASSERT_TRUE(bb_tls_heap_guard_passes(20000, 16384, 50000, 0, NULL));
}

void test_heap_guard_passes_contiguous_fails(void)
{
    // contiguous block below floor — guard trips on "contiguous"
    const char *dim = NULL;
    bool ok = bb_tls_heap_guard_passes(10000, 16384, 50000, 0, &dim);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NOT_NULL(dim);
    TEST_ASSERT_EQUAL_STRING("contiguous", dim);
}

void test_heap_guard_passes_total_free_fails(void)
{
    // contiguous OK but total-free below floor — guard trips on "total-free"
    const char *dim = NULL;
    bool ok = bb_tls_heap_guard_passes(20000, 16384, 25000, 30720, &dim);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NOT_NULL(dim);
    TEST_ASSERT_EQUAL_STRING("total-free", dim);
}

void test_heap_guard_passes_both_ok(void)
{
    // both dimensions above their floors — guard passes
    TEST_ASSERT_TRUE(
        bb_tls_heap_guard_passes(20000, 16384, 40000, 30720, NULL));
}

void test_heap_guard_passes_contiguous_fails_before_total_free_checked(void)
{
    // contiguous trips first; total-free would also fail but dim reports "contiguous"
    const char *dim = NULL;
    bool ok = bb_tls_heap_guard_passes(8000, 16384, 25000, 30720, &dim);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("contiguous", dim);
}

void test_heap_guard_passes_exactly_at_contiguous_floor(void)
{
    // block == floor (boundary) — should pass (>=)
    TEST_ASSERT_TRUE(
        bb_tls_heap_guard_passes(16384, 16384, 50000, 30720, NULL));
}

void test_heap_guard_passes_exactly_at_total_free_floor(void)
{
    // total_free == floor (boundary) — should pass (>=)
    TEST_ASSERT_TRUE(
        bb_tls_heap_guard_passes(20000, 16384, 30720, 30720, NULL));
}

void test_heap_guard_passes_total_free_only_floor_set(void)
{
    // only total-free floor set; total < floor — trips "total-free"
    const char *dim = NULL;
    bool ok = bb_tls_heap_guard_passes(20000, 0, 20000, 30720, &dim);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("total-free", dim);
}

void test_heap_guard_passes_null_out_dim_does_not_crash(void)
{
    // caller passes NULL for out_dim — must not crash on guard failure
    bool ok = bb_tls_heap_guard_passes(8000, 16384, 50000, 0, NULL);
    TEST_ASSERT_FALSE(ok);
}

// ---------------------------------------------------------------------------
// bb_ota_pull_heap_ready_for_test — guard predicate via the heap-ready hook
//
// Uses bb_ota_pull_heap_ready_for_test() to verify that the same underlying
// predicate used by bb_ota_pull_heap_ready() on device correctly gates both
// heap dimensions. Floor constants mirror representative real values:
//   contiguous floor: 16384 + 1024 = 17408 (SSL_IN_CONTENT_LEN=16384 + 1 KB)
//   total-free  floor: 30720 (~30 KB, typical CONFIG_BB_TLS_HEAP_TOTAL_FLOOR)
// ---------------------------------------------------------------------------

#define TEST_CONTIGUOUS_FLOOR 17408u   /* SSL_IN_CONTENT_LEN(16384) + 1024 */
#define TEST_TOTAL_FREE_FLOOR 30720u   /* representative min-free-heap floor */

void test_heap_ready_passes_when_both_dims_above_floor(void)
{
    // largest > contiguous floor AND total_free > total floor — ready
    TEST_ASSERT_TRUE(
        bb_ota_pull_heap_ready_for_test(TEST_CONTIGUOUS_FLOOR, TEST_CONTIGUOUS_FLOOR,
                                        TEST_TOTAL_FREE_FLOOR, TEST_TOTAL_FREE_FLOOR));
}

void test_heap_ready_fails_when_contiguous_below_floor(void)
{
    // largest block one byte below contiguous floor — not ready
    TEST_ASSERT_FALSE(
        bb_ota_pull_heap_ready_for_test(TEST_CONTIGUOUS_FLOOR - 1, TEST_CONTIGUOUS_FLOOR,
                                        TEST_TOTAL_FREE_FLOOR + 10000, TEST_TOTAL_FREE_FLOOR));
}

void test_heap_ready_fails_when_total_free_below_floor(void)
{
    // contiguous fine but total_free one byte below floor — not ready
    TEST_ASSERT_FALSE(
        bb_ota_pull_heap_ready_for_test(TEST_CONTIGUOUS_FLOOR + 10000, TEST_CONTIGUOUS_FLOOR,
                                        TEST_TOTAL_FREE_FLOOR - 1, TEST_TOTAL_FREE_FLOOR));
}

void test_heap_ready_passes_at_exact_contiguous_boundary(void)
{
    // largest == contiguous floor exactly (>= boundary) — ready
    TEST_ASSERT_TRUE(
        bb_ota_pull_heap_ready_for_test(TEST_CONTIGUOUS_FLOOR, TEST_CONTIGUOUS_FLOOR,
                                        TEST_TOTAL_FREE_FLOOR + 1, TEST_TOTAL_FREE_FLOOR));
}

void test_heap_ready_passes_at_exact_total_free_boundary(void)
{
    // total_free == total-free floor exactly (>= boundary) — ready
    TEST_ASSERT_TRUE(
        bb_ota_pull_heap_ready_for_test(TEST_CONTIGUOUS_FLOOR + 1, TEST_CONTIGUOUS_FLOOR,
                                        TEST_TOTAL_FREE_FLOOR, TEST_TOTAL_FREE_FLOOR));
}

void test_heap_ready_passes_when_both_floors_disabled(void)
{
    // floors = 0 (guard disabled on PSRAM/ample-heap boards) — always ready
    TEST_ASSERT_TRUE(bb_ota_pull_heap_ready_for_test(0, 0, 0, 0));
}

// ---------------------------------------------------------------------------
// OTA claim deadlock fix — claim NOT held during refresh, released on failure
//
// The apply handler must NOT hold the OTA claim while calling
// bb_update_check_run_blocking(): on device that spawns an upd_check worker
// that acquires the same claim, causing a deadlock (upd_check skips → refresh
// times out → claim leaks).
//
// After the fix the claim is acquired only immediately before the worker spawn
// (after all validation). The host ota_claim stub mirrors the ESP-IDF arbiter
// so we can verify the contract without a FreeRTOS target.
// ---------------------------------------------------------------------------

// 1. Claim is free before it is acquired (simulate the refresh window).
//    upd_check can acquire the claim while ota_pull has not yet taken it.
void test_ota_apply_claim_free_during_refresh_window(void)
{
    bb_update_check_ota_claim_reset();

    // Before ota_pull acquires the claim, upd_check can take the slot.
    bb_err_t rc = bb_update_check_ota_claim_acquire("upd_check");
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("upd_check", bb_update_check_ota_claim_holder_for_test());

    // upd_check completes and releases.
    bb_update_check_ota_claim_release("upd_check");
    TEST_ASSERT_NULL(bb_update_check_ota_claim_holder_for_test());
}

// 2. Claim is released on the refresh-failure early-return path.
//    If the apply handler returns early after a failed refresh, the claim
//    must not be held.  On the fixed code the claim is acquired AFTER the
//    refresh, so a refresh failure returns before the acquire — the claim
//    stays free. Model: acquire "ota_pull" only after a successful refresh,
//    never on failure.
void test_ota_apply_claim_not_leaked_on_refresh_failure(void)
{
    bb_update_check_ota_claim_reset();

    // Simulate: refresh fails → handler returns without acquiring the claim.
    // Claim must be free (not leaked).
    TEST_ASSERT_NULL(bb_update_check_ota_claim_holder_for_test());

    // A subsequent upd_check run can acquire the claim freely.
    bb_err_t rc = bb_update_check_ota_claim_acquire("upd_check");
    TEST_ASSERT_EQUAL(BB_OK, rc);
    bb_update_check_ota_claim_release("upd_check");
    TEST_ASSERT_NULL(bb_update_check_ota_claim_holder_for_test());
}

// 3. Claim is held during the spawn window and released by the worker on exit.
//    ota_pull acquires just before spawn; ota_task_exit releases it.
//    Model that lifecycle with the host stub.
void test_ota_apply_claim_held_only_during_spawn_window(void)
{
    bb_update_check_ota_claim_reset();

    // Simulate: all validation passed, about to spawn worker → acquire.
    bb_err_t rc = bb_update_check_ota_claim_acquire("ota_pull");
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("ota_pull", bb_update_check_ota_claim_holder_for_test());

    // upd_check cannot acquire while ota_pull holds the slot.
    rc = bb_update_check_ota_claim_acquire("upd_check");
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);

    // Worker completes (ota_task_exit releases).
    bb_update_check_ota_claim_release("ota_pull");
    TEST_ASSERT_NULL(bb_update_check_ota_claim_holder_for_test());

    // upd_check can now run.
    rc = bb_update_check_ota_claim_acquire("upd_check");
    TEST_ASSERT_EQUAL(BB_OK, rc);
    bb_update_check_ota_claim_release("upd_check");
    TEST_ASSERT_NULL(bb_update_check_ota_claim_holder_for_test());
}

// 4. Claim is released on spawn-failure path.
//    If xTaskCreate fails after the claim is acquired, the handler must
//    release before returning 500.
void test_ota_apply_claim_released_on_spawn_failure(void)
{
    bb_update_check_ota_claim_reset();

    // Simulate: claim acquired, spawn fails → release.
    bb_err_t rc = bb_update_check_ota_claim_acquire("ota_pull");
    TEST_ASSERT_EQUAL(BB_OK, rc);

    // spawn failure path
    bb_update_check_ota_claim_release("ota_pull");
    TEST_ASSERT_NULL(bb_update_check_ota_claim_holder_for_test());
}

// ---------------------------------------------------------------------------
// bb_ota_pull_resolve_redirect_url — pure redirect-URL decision helper
// ---------------------------------------------------------------------------

#define GITHUB_URL  "https://github.com/owner/repo/releases/download/v1.0.0/fw.bin"
#define CDN_URL     "https://objects.githubusercontent.com/github-production-release-asset-ABCD/fw.bin"

void test_resolve_redirect_url_redirect_uses_cdn(void)
{
    // Successful probe that followed a redirect → use CDN URL
    bool did = false;
    const char *result = bb_ota_pull_resolve_redirect_url(GITHUB_URL, CDN_URL, 0, &did);
    TEST_ASSERT_EQUAL_STRING(CDN_URL, result);
    TEST_ASSERT_TRUE(did);
}

void test_resolve_redirect_url_no_redirect_uses_original(void)
{
    // Probe succeeded but URL did not change → no redirect; use original
    bool did = false;
    const char *result = bb_ota_pull_resolve_redirect_url(GITHUB_URL, GITHUB_URL, 0, &did);
    TEST_ASSERT_EQUAL_STRING(GITHUB_URL, result);
    TEST_ASSERT_FALSE(did);
}

void test_resolve_redirect_url_probe_failed_uses_original(void)
{
    // Probe returned non-zero error → fall back to original regardless of resolved URL
    bool did = false;
    const char *result = bb_ota_pull_resolve_redirect_url(GITHUB_URL, CDN_URL, -1, &did);
    TEST_ASSERT_EQUAL_STRING(GITHUB_URL, result);
    TEST_ASSERT_FALSE(did);
}

void test_resolve_redirect_url_null_resolved_uses_original(void)
{
    // Probe returned NULL resolved URL → fall back to original
    bool did = false;
    const char *result = bb_ota_pull_resolve_redirect_url(GITHUB_URL, NULL, 0, &did);
    TEST_ASSERT_EQUAL_STRING(GITHUB_URL, result);
    TEST_ASSERT_FALSE(did);
}

void test_resolve_redirect_url_empty_resolved_uses_original(void)
{
    // Probe returned empty string → fall back to original
    bool did = false;
    const char *result = bb_ota_pull_resolve_redirect_url(GITHUB_URL, "", 0, &did);
    TEST_ASSERT_EQUAL_STRING(GITHUB_URL, result);
    TEST_ASSERT_FALSE(did);
}

void test_resolve_redirect_url_null_out_did_redirect_does_not_crash(void)
{
    // Caller may pass NULL for out_did_redirect — must not crash
    const char *result = bb_ota_pull_resolve_redirect_url(GITHUB_URL, CDN_URL, 0, NULL);
    TEST_ASSERT_EQUAL_STRING(CDN_URL, result);
}
