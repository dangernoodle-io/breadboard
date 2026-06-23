// bb_pub_mem — telemetry source satellite: live memory metrics.
//
// Registers a bb_pub source under the "mem" subtopic. On each tick, samples
// bb_board heap accessors and emits:
//   heap_internal_free          size_t (bytes)
//   heap_internal_min_free      size_t (bytes)
//   heap_internal_largest_block size_t (bytes)
//   psram_free                  size_t (bytes), omitted when no PSRAM hardware
//   uptime_ms                   uint32_t (milliseconds)
//
// Always returns true (memory is always present). psram_free is omitted
// (not null) when bb_board_psram_total() == 0.
//
// Self-registration is gated on CONFIG_BB_PUB_MEM_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP tier
// at an order after bb_pub so the source registry exists first.
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "mem" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 * Called automatically at PRE_HTTP tier when CONFIG_BB_PUB_MEM_AUTO_ATTACH=y.
 */
bb_err_t bb_pub_mem_register(void);

#ifdef __cplusplus
}
#endif
