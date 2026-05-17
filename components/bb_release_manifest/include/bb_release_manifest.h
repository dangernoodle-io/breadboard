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

// ---------------------------------------------------------------------------
// Streaming GitHub parser
//
// Usage:
//   bb_release_manifest_stream_ctx_t ctx;
//   bb_release_manifest_parse_github_stream_begin(&ctx, board, tag, sizeof(tag), url, sizeof(url));
//   // for each chunk received:
//   bb_release_manifest_parse_github_stream_feed(&ctx, data, len);
//   bb_release_manifest_parse_github_stream_end(&ctx);
//
// The _state[] array is opaque; allocate the struct on the stack. Its size
// is chosen to hold all parser state fields with alignment padding.
//
// Error propagation: _feed accumulates the first non-BB_OK error internally.
// _end returns BB_ERR_NOT_FOUND if the required fields were not found, or the
// first error from _feed if one occurred.
// ---------------------------------------------------------------------------

// Internal state size: parser fields + board_fallback copy + output pointers.
// 512 bytes is sufficient; see bb_release_manifest_github_stream.c for layout.
#define BB_RELEASE_MANIFEST_STREAM_STATE_SIZE 512

typedef struct bb_release_manifest_stream_ctx bb_release_manifest_stream_ctx_t;
struct bb_release_manifest_stream_ctx {
    char _state[BB_RELEASE_MANIFEST_STREAM_STATE_SIZE];
};

/**
 * Initialize a streaming GitHub release manifest parser context.
 *
 * All output pointers are borrowed — they must remain valid through _end().
 *
 * @param ctx            Parser context allocated by caller (stack is fine)
 * @param board_fallback Board name to match (e.g. "taipanminer-bitaxe-650")
 * @param tag_out        Output buffer for tag_name
 * @param tag_cap        Capacity of tag_out (min 2)
 * @param url_out        Output buffer for browser_download_url
 * @param url_cap        Capacity of url_out (min 2)
 * @return BB_OK or BB_ERR_INVALID_ARG on NULL/zero-cap args
 */
bb_err_t bb_release_manifest_parse_github_stream_begin(
    bb_release_manifest_stream_ctx_t *ctx,
    const char *board_fallback,
    char *tag_out, size_t tag_cap,
    char *url_out, size_t url_cap);

/**
 * Feed a chunk of JSON bytes into the streaming parser.
 *
 * May be called with any chunk size including 1 byte.
 * Returns BB_OK to continue, or an error to signal abort to the transport.
 * The context retains the first error; subsequent _feed calls are no-ops.
 *
 * @param ctx       Initialized context
 * @param chunk     Pointer to data bytes (need not be null-terminated)
 * @param chunk_len Number of bytes in chunk
 */
bb_err_t bb_release_manifest_parse_github_stream_feed(
    bb_release_manifest_stream_ctx_t *ctx,
    const char *chunk, size_t chunk_len);

/**
 * Finalize the parse. Must be called once after all chunks are fed.
 *
 * @return BB_OK if both tag_name and the matching asset URL were extracted,
 *         BB_ERR_NOT_FOUND if either was missing,
 *         BB_ERR_NO_SPACE if a buffer overflow occurred during extraction,
 *         or the first error returned by _feed.
 */
bb_err_t bb_release_manifest_parse_github_stream_end(
    bb_release_manifest_stream_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
