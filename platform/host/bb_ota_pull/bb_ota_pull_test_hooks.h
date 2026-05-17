#pragma once

#include <stdint.h>
#include <stddef.h>
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
