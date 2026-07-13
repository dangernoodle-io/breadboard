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

// Internal state size: the shared bb_release_manifest_sink_state_t
// (extraction fields + board_fallback/name-match scratch), the
// bb_serialize_json_ingest_t vtable bound to it, and an embedded
// bb_serialize_json_scan_ctx_t (the shared JSON scanner's own 384-byte
// opaque state, see bb_serialize_json.h) -- 672 bytes (216 + 72 + 384) on
// a 64-bit host build. The host figure OVERSTATES the device cost: on a
// 32-bit target, every pointer/size_t field halves, so the sink shrinks
// 216 -> 184 and the 9-entry vtable (1 ctx pointer + 8 function pointers)
// shrinks 72 -> 36, while the scanner's own state stays a fixed 384 (it's
// declared as a byte array, not pointer-sized fields) -- real 32-bit
// total is ~604 bytes (184 + 36 + 384), not 672. Sized to 768 for
// headroom above the 64-bit host figure (host is the tighter case here),
// not shaved down to exactly what fits today (same convention as
// BB_SERIALIZE_JSON_SCAN_STATE_SIZE). This grew from the pre-scanner
// state machine's 512 bytes when bb_release_manifest_github_stream.c was
// rewritten onto the shared bb_serialize_json scanner (PR3 of the JSON
// scanner arc) -- a deliberate, disclosed public struct-size change
// (device: +~90-100 bytes over the old 512, not the host figure's
// +256), not an oversight. bb_ota_check_common.c's bb_ota_check_run_one()
// stack-allocates a stream_feed_ctx_t wrapping this (~772 bytes total)
// inside the on-demand update-check worker task, whose stack is
// BB_HTTP_CLIENT_TASK_STACK (default 8192 bytes, see
// components/bb_http_client/include/bb_http_client.h) -- under 10% of
// that budget, with ample headroom left for the mbedTLS handshake that
// dominates the rest of that task's stack use. See
// bb_release_manifest_github_stream.c for the exact layout; a compile-time
// _Static_assert there catches this ever falling short.
#define BB_RELEASE_MANIFEST_STREAM_STATE_SIZE 768

typedef struct bb_release_manifest_stream_ctx bb_release_manifest_stream_ctx_t;
struct bb_release_manifest_stream_ctx {
    char _state[BB_RELEASE_MANIFEST_STREAM_STATE_SIZE];
};

/**
 * Initialize a streaming GitHub release manifest parser context.
 *
 * All output pointers are borrowed — they must remain valid through _end().
 * tag_out/url_out contents are UNSPECIFIED until _end() returns -- do not
 * read them between _feed() calls or before _end() has been called; a
 * caller polling mid-stream could observe a partial value that _end() has
 * not yet had a chance to clear.
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
