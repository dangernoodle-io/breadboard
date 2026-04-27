#pragma once

#include <stdint.h>

/**
 * Host-only test hook: returns the current per-recv HTTP timeout (ms).
 * Used by unit tests to verify bb_ota_pull_set_http_timeout_ms().
 * Not available on device builds.
 */
uint32_t bb_ota_pull_host_get_http_timeout_ms(void);
