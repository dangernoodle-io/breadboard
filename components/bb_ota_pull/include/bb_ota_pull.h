#pragma once

#include <stddef.h>
#include <stdbool.h>

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
 * Parse a GitHub releases/latest JSON response and extract the latest tag
 * and asset download URL for the given board.
 *
 * Platform-independent implementation, testable on host.
 *
 * @param json       Full JSON response body
 * @param board_name Board name to match (e.g. "tdongle-s3")
 * @param out_tag    Buffer for tag_name (min 32 bytes)
 * @param tag_size   Size of out_tag buffer
 * @param out_url    Buffer for browser_download_url (min 256 bytes)
 * @param url_size   Size of out_url buffer
 * @return 0 on success, -1 if tag not found, -2 if no matching asset
 */
int bb_ota_pull_parse_release_json(const char *json, const char *board_name,
                                     char *out_tag, size_t tag_size,
                                     char *out_url, size_t url_size);

#ifdef ESP_PLATFORM
#include "bb_nv.h"
#include "bb_http.h"

/**
 * Register OTA pull HTTP handlers with an existing HTTP server.
 * Adds GET /api/ota/check, POST /api/ota/update, and GET /api/ota/status.
 */
bb_err_t bb_ota_pull_register_handler(bb_http_handle_t server);

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
