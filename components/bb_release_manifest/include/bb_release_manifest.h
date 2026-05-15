#pragma once

#include "bb_core.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback signature for GitHub release manifest parsers.
 * Parses a release manifest JSON response and extracts tag + asset URL.
 *
 * @param body       Release manifest body (may be binary or contain null bytes)
 * @param body_len   Length of body in bytes
 * @param board_name Board/asset name to match (e.g. "tdongle-s3")
 * @param out_tag    Buffer for tag_name (min 32 bytes)
 * @param tag_size   Size of out_tag buffer
 * @param out_url    Buffer for browser_download_url (min 256 bytes)
 * @param url_size   Size of out_url buffer
 * @return BB_OK on success, BB_ERR_NOT_FOUND if tag or asset not found,
 *         BB_ERR_INVALID_ARG if any required arg is NULL or size is 0
 */
typedef bb_err_t (*bb_release_manifest_parse_fn)(
    const char *body, size_t body_len,
    const char *board_name,
    char *out_tag, size_t tag_size,
    char *out_url, size_t url_size);

/**
 * Parse a GitHub releases/latest JSON response and extract the latest tag
 * and asset download URL for the given board.
 *
 * Platform-independent implementation, testable on host.
 *
 * @param body       Full JSON response body
 * @param body_len   Length of body in bytes
 * @param board_name Board name to match (e.g. "tdongle-s3")
 * @param out_tag    Buffer for tag_name (min 32 bytes)
 * @param tag_size   Size of out_tag buffer
 * @param out_url    Buffer for browser_download_url (min 256 bytes)
 * @param url_size   Size of out_url buffer
 * @return BB_OK on success, BB_ERR_NOT_FOUND if tag or asset not found,
 *         BB_ERR_INVALID_ARG if any required arg is NULL or size is 0
 */
bb_err_t bb_release_manifest_parse_github(
    const char *body, size_t body_len,
    const char *board_name,
    char *out_tag, size_t tag_size,
    char *out_url, size_t url_size);

#ifdef __cplusplus
}
#endif
