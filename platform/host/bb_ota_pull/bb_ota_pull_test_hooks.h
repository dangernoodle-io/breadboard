#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "bb_core.h"

/**
 * Host-only test hook: returns the current per-recv HTTP timeout (ms).
 * Used by unit tests to verify bb_ota_pull_set_http_timeout_ms().
 * Not available on device builds.
 */
uint32_t bb_ota_pull_host_get_http_timeout_ms(void);

/**
 * Test hook: run ota_fetch_manifest with the currently configured URL/board.
 * Available when BB_OTA_PULL_TESTING is defined. Exercises the streaming
 * manifest-fetch path from host unit tests.
 */
bb_err_t bb_ota_pull_fetch_manifest_for_test(char *out_tag, size_t tag_cap,
                                             char *out_url, size_t url_cap);

/**
 * Pure retry-decision helper (portable, no ESP-IDF types).
 * Returns true when a download attempt should be retried.
 * @param perform_err   last error from esp_https_ota_perform() (0 = OK)
 * @param data_complete result of esp_https_ota_is_complete_data_received()
 */
bool bb_ota_pull_download_should_retry(int perform_err, bool data_complete);

/**
 * Pure redirect-URL decision helper (portable, no ESP-IDF types).
 *
 * Given the original URL, the resolved URL from an HTTP redirect probe, and the
 * probe's perform result, returns which URL the OTA download should use.
 *
 * Rules (in priority order):
 *   1. If perform_err != 0 → use original; out_did_redirect = false.
 *   2. If resolved_url is NULL or empty → use original; out_did_redirect = false.
 *   3. If resolved_url == original_url → no redirect; use original.
 *   4. Otherwise → redirect followed; use resolved_url; out_did_redirect = true.
 *
 * Never returns NULL.
 *
 * @param original_url     the initial asset URL (must not be NULL)
 * @param resolved_url     URL after HTTP client performed (may be NULL)
 * @param perform_err      0 on success, non-zero on failure
 * @param out_did_redirect set to true when a cross-host redirect was followed
 * @return the URL to use for the OTA download
 */
const char *bb_ota_pull_resolve_redirect_url(const char *original_url,
                                             const char *resolved_url,
                                             int perform_err,
                                             bool *out_did_redirect);

/**
 * Pure pre-flight heap guard predicate (portable, no ESP-IDF types).
 * Returns true when the guard passes (OTA/check may proceed), false when
 * either heap dimension is below its configured floor.
 *
 * @param largest_block    measured largest contiguous free block (bytes)
 * @param contiguous_floor minimum required contiguous block (0 = disabled)
 * @param total_free       measured total free heap (bytes)
 * @param total_floor      minimum required total free heap (0 = disabled)
 * @param out_dim          on failure, points to "contiguous" or "total-free";
 *                         may be NULL when the label is not needed
 */
bool bb_ota_pull_heap_guard_passes(size_t largest_block, size_t contiguous_floor,
                                   size_t total_free, size_t total_floor,
                                   const char **out_dim);

/**
 * Test hook: exercise bb_ota_pull_heap_ready's predicate with synthetic heap
 * values and caller-supplied floor constants. Use this from host unit tests
 * since the real bb_ota_pull_heap_ready() is ESP-IDF-only (calls heap_caps_*).
 *
 * Available when BB_OTA_PULL_TESTING is defined.
 */
bool bb_ota_pull_heap_ready_for_test(size_t largest_block, size_t contiguous_floor,
                                     size_t total_free, size_t total_floor);
