#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"

/**
 * Set the GitHub releases URL for fetching updates.
 * Default: https://api.github.com/repos/dangernoodle-io/snugfeather/releases/latest
 * @param url URL to set (or NULL to reset to default)
 */
void bb_ota_pull_set_releases_url(const char *url);

/**
 * Set the per-recv HTTP timeout used for the OTA download.
 * Default 20000 ms. Pass 0 to restore the default. Must be called
 * before the OTA task is started; later calls take effect on the
 * next OTA invocation.
 */
void bb_ota_pull_set_http_timeout_ms(uint32_t ms);

/**
 * Pure freshness predicate for the POST /api/update/apply cache-first logic.
 *
 * Returns true when the cached update-check result is fresh enough to trust:
 *   - last_check_ok must be true (a successful check ran)
 *   - (now_us - last_check_us) must be <= window_s * 1e6
 *   - window_s == 0 always returns false (always refresh)
 *
 * No ESP-IDF dependencies; exercised on host by unit tests.
 */
bool bb_ota_pull_apply_cache_is_fresh(bool last_check_ok, int64_t last_check_us,
                                      int64_t now_us, int32_t window_s);

#ifdef ESP_PLATFORM
#include "bb_http.h"

/**
 * Sample internal-cap heap and return true when the pre-flight guard would
 * pass — i.e. the board currently has enough contiguous internal RAM for the
 * TLS handshake and enough total free internal RAM for the full OTA session.
 *
 * This is the same predicate used by the OTA download path (single source of
 * truth). Call it before attempting an OTA to surface an early "ota_ready"
 * signal without actually starting a download.
 *
 * Returns false on no-PSRAM boards whose heap is already fragmented below the
 * guard floors; returns true on PSRAM boards or boards with ample internal RAM.
 */
bool bb_ota_pull_heap_ready(void);

/**
 * Trigger an immediate OTA check (non-blocking).
 * Results can be queried via GET /api/ota/check.
 */
bb_err_t bb_ota_pull_check_now(void);

/**
 * Configure which FreeRTOS core the OTA worker tasks run on.
 * Default: 1. Pass tskNO_AFFINITY (-1) to let FreeRTOS schedule.
 * Some boards (e.g. tdongle-s3 without ASIC offload) hit an esp-idf DVFS race
 * when the OTA flash-write stalls the other core; set to 0 or tskNO_AFFINITY
 * on those boards.
 */
void bb_ota_pull_set_task_core(int core);

/**
 * Configure the OTA worker task priority. Default: 3.
 * On single-core targets the worker must outrank a CPU-bound consumer task
 * (e.g. a software-mining hot loop) so it can preempt and invoke the pause hook;
 * otherwise the sustained download starves the idle task and trips the task WDT.
 * Mirrors bb_ota_check_set_task_priority.
 */
void bb_ota_pull_set_task_priority(int priority);

/**
 * Download + flash a firmware image synchronously on the CALLING task (no
 * worker spawn). Returns BB_OK when the new image is written and the caller
 * should reboot, else an error. Intended for OTA-only boot mode, where the full
 * heap is available because no subsystems have started. Does not pause/resume
 * work and does not reboot — the caller owns that.
 */
bb_err_t bb_ota_pull_run_sync(const char *asset_url);

/* Reserve route-table slots for bb_ota_pull before the HTTP server starts. */
// bbtool:init tier=pre_http fn=bb_ota_pull_reserve_routes
bb_err_t bb_ota_pull_reserve_routes(void);

// POST /api/update/apply (plus /api/update/check + /api/update/progress) has
// a single registrant, chosen by the BB_OTA_STRATEGY Kconfig choice
// (components/bb_core/Kconfig) -- bb_ota_pull and bb_ota_boot must never
// both register the route. codegen's `// bbtool:init` marker scan is
// grep-time / preprocessor-unaware (see wire_parse.py), so the marker is
// only visible (and only resolves to the real function) when this strategy
// is selected; otherwise the no-op stub below satisfies the generated call
// and registers nothing. Mirrors the bb_cache_evict_start Kconfig-bridge
// stub pattern (bb_cache.h).
#if defined(CONFIG_BB_OTA_STRATEGY_PULL) && CONFIG_BB_OTA_STRATEGY_PULL
/**
 * Register OTA pull HTTP handlers with an existing httpd instance.
 */
// bbtool:init tier=regular fn=bb_ota_pull_init server=true
bb_err_t bb_ota_pull_init(bb_http_handle_t server);
#else
static inline bb_err_t bb_ota_pull_init(bb_http_handle_t server) { (void)server; return BB_OK; }
#endif

#endif // ESP_PLATFORM
