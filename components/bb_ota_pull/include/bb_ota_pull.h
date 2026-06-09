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
 * Mirrors bb_update_check_set_task_priority.
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

#endif // ESP_PLATFORM
