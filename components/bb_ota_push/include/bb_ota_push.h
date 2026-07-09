#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "bb_core.h"   // bb_ota_progress_cb_t / bb_ota_phase_t

/**
 * Validate Content-Length for an OTA push request.
 * Returns 0 if valid, 400 if content_len <= 0, 413 if content_len > max_size.
 * Exposed for host unit testing; called internally by the HTTP handler.
 */
int bb_ota_push_validate_content_len(int content_len, int max_size);

/**
 * Compute the maximum wall-clock time (ms) allowed for the whole OTA receive,
 * derived from a minimum acceptable average throughput. Clamped to
 * [floor_ms, ceil_ms]. ceil_ms MUST be < the extended WDT window so the abort
 * beats the watchdog. Exposed outside ESP_PLATFORM for host unit testing.
 */
uint32_t bb_ota_push_deadline_ms(int content_len, int min_bytes_per_sec,
                                 uint32_t floor_ms, uint32_t ceil_ms);

#ifdef BB_OTA_PUSH_TESTING
uint32_t bb_ota_push_deadline_ms_for_test(int content_len, int min_bytes_per_sec,
                                          uint32_t floor_ms, uint32_t ceil_ms);
#endif

#ifdef ESP_PLATFORM
#include "bb_nv.h"
#include "bb_http.h"

/* Reserve route-table slots for bb_ota_push before the HTTP server starts. */
// bbtool:init tier=pre_http fn=bb_ota_push_reserve_routes
bb_err_t bb_ota_push_reserve_routes(void);

/**
 * Register OTA push HTTP handler with an existing httpd instance.
 */
// bbtool:init tier=regular fn=bb_ota_push_init server=true
bb_err_t bb_ota_push_init(bb_http_handle_t server);

#endif // ESP_PLATFORM
