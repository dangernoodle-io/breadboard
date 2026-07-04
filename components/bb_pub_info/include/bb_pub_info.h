// bb_pub_info — telemetry source satellite for device info / system metrics.
//
// Registers a bb_pub source under the "info" subtopic. On each tick, emits:
//   heap_internal_free            integer (bytes)
//   heap_internal_total           integer (bytes)
//   heap_internal_largest_block   integer (bytes)
//   heap_internal_min_free        integer (bytes)
//   psram_free                    integer (bytes; omitted when no PSRAM)
//   psram_total                   integer (bytes; omitted when no PSRAM)
//   wdt_resets                    integer (abnormal-reset count)
//   ota_validated                 boolean
//   time_valid                    boolean
//   bb_mem_out                    integer (outstanding bytes; 0 when BB_MEM_STATS_ENABLE off)
//   bb_mem_peak                   integer (peak outstanding; 0 when BB_MEM_STATS_ENABLE off)
//   bb_mem_fail                   integer (cumulative alloc failures; 0 when off)
//
// Note: ts_ms (sample-time epoch ms) is no longer emitted here — bb_cache
// owns it and applies it as the wire envelope ({"ts_ms":N,"data":{...}})
// at the two serialize points (B1-570 PR-3).
//
// Note: static device-identity fields (version, board, chip_model, mac,
// flash_size, app_size, dram_static_bytes, reset_reason, boot_epoch_s,
// time_source, rtc_used, rtc_total) have moved to the meta topic (TA-505).
//
// This source always publishes (never skips), providing a heartbeat even
// when no hardware HALs are present.
//
// Self-registration is gated on CONFIG_BB_PUB_INFO_AUTO_ATTACH (default y,
// depends on BB_PUB_AUTOREGISTER). Registration happens at the PRE_HTTP tier
// at an order after bb_pub so the source registry exists first.
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "info" telemetry source with bb_pub.
 * Idempotent — subsequent calls are no-ops (source slot already taken).
 * Called automatically at PRE_HTTP tier when CONFIG_BB_PUB_INFO_AUTO_ATTACH=y.
 */
bb_err_t bb_pub_info_register(void);

#ifdef __cplusplus
}
#endif
