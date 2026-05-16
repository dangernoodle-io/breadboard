#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * Callback invoked before OTA apply to pause work.
 * @return true if caller paused work, false otherwise
 */
typedef bool (*bb_ota_pause_cb_t)(void);

/**
 * Callback invoked after OTA apply to resume work.
 */
typedef void (*bb_ota_resume_cb_t)(void);

/**
 * Callback to skip project-name mismatch check.
 * @return true to skip check and proceed with OTA, false to abort on mismatch
 */
typedef bool (*bb_ota_skip_check_cb_t)(void);

/**
 * Set optional callbacks for pausing/resuming work during OTA.
 * If unset, OTA proceeds without pause.
 */
void bb_ota_pull_set_hooks(bb_ota_pause_cb_t pause, bb_ota_resume_cb_t resume);

/**
 * Set optional callback to skip project-name mismatch check.
 * Allows consumers to override firmware board mismatch validation if needed.
 */
void bb_ota_pull_set_skip_check_cb(bb_ota_skip_check_cb_t cb);

/**
 * Set the GitHub releases URL for fetching updates.
 * Default: https://api.github.com/repos/dangernoodle-io/snugfeather/releases/latest
 * @param url URL to set (or NULL to reset to default)
 */
void bb_ota_pull_set_releases_url(const char *url);

/**
 * Set the firmware board name/prefix for asset lookup.
 * Default: read from FIRMWARE_BOARD cmake define
 * @param board Board name (e.g. "tdongle-s3")
 */
void bb_ota_pull_set_firmware_board(const char *board);

/**
 * Set the per-recv HTTP timeout used for the OTA download.
 * Default 20000 ms. Pass 0 to restore the default. Must be called
 * before the OTA task is started; later calls take effect on the
 * next OTA invocation.
 */
void bb_ota_pull_set_http_timeout_ms(uint32_t ms);

#ifdef ESP_PLATFORM
#include "bb_nv.h"
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

#endif // ESP_PLATFORM
