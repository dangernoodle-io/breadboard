#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum usable SSID length (chars, excluding NUL).
 * Matches esp wifi_config_t sta.ssid usable bytes minus NUL.
 */
#define BB_WIFI_PENDING_SSID_MAX 31

/**
 * Maximum usable password length (chars, excluding NUL).
 * Matches esp wifi_config_t sta.password usable bytes minus NUL.
 */
#define BB_WIFI_PENDING_PASS_MAX 63

/**
 * Outcome of bb_wifi_pending_decide().
 *
 * BB_WIFI_PENDING_NONE - no pending reconfigure attempt should be made.
 * BB_WIFI_PENDING_TRY  - staged creds are valid; attempt a reconfigure.
 */
typedef enum {
    BB_WIFI_PENDING_NONE = 0,
    BB_WIFI_PENDING_TRY  = 1,
} bb_wifi_pending_action_t;

/**
 * Decide whether to attempt a pending WiFi reconfigure.
 *
 * Returns BB_WIFI_PENDING_TRY iff try_flag != 0 AND pending_ssid is
 * non-NULL and non-empty. Returns BB_WIFI_PENDING_NONE otherwise.
 *
 * @param try_flag     NVS try flag value (0 = no pending attempt).
 * @param pending_ssid Staged SSID read from NVS; may be NULL or empty.
 */
bb_wifi_pending_action_t bb_wifi_pending_decide(uint8_t try_flag,
                                                const char *pending_ssid);

/**
 * Validate proposed WiFi credentials before staging.
 *
 * ssid must be non-NULL, non-empty, and at most BB_WIFI_PENDING_SSID_MAX
 * characters. pass may be NULL (treated as empty string — open network);
 * if non-NULL it must be at most BB_WIFI_PENDING_PASS_MAX characters.
 *
 * @param ssid  Proposed SSID; NULL or empty -> BB_ERR_INVALID_ARG.
 * @param pass  Proposed password; NULL treated as empty (OK).
 * @return BB_OK on success, BB_ERR_INVALID_ARG on validation failure.
 */
bb_err_t bb_wifi_pending_validate(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
