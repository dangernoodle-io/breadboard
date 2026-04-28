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
typedef bool (*bb_ota_push_skip_check_cb_t)(void);

/**
 * Set optional callbacks for pausing/resuming work during OTA.
 * If unset, OTA proceeds without pause.
 */
void bb_ota_push_set_hooks(bb_ota_pause_cb_t pause, bb_ota_resume_cb_t resume);

/**
 * Set optional callback to skip project-name mismatch check.
 * Allows consumers to override firmware board mismatch validation if needed.
 */
void bb_ota_push_set_skip_check_cb(bb_ota_push_skip_check_cb_t cb);

#ifdef ESP_PLATFORM
#include "bb_nv.h"
#include "bb_http.h"

#endif // ESP_PLATFORM
