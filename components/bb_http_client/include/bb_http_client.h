#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// bb_http_client — portable outbound HTTP GET wrapper.
//
// Two fetch modes:
//   bb_http_client_get        — buffers the entire response; caller-owned buffer.
//   bb_http_client_get_stream — streams body chunks to a callback; no body buffer.
//
// Implementations:
//   - ESP-IDF: esp_http_client with TLS via the ESP cert bundle.
//   - Host:    pthread mock; tests register a response via the test hook in
//              the host port header. Real network access is not implemented
//              on host (intentionally — host tests must be hermetic).
//   - Arduino: stub returning BB_ERR_UNSUPPORTED.

typedef struct {
    uint32_t timeout_ms;       // 0 -> 10000 (10 s)
    uint8_t  max_attempts;     // 0 -> 3
    uint16_t buffer_size;      // internal client receive buffer; 0 -> 4096
    const char *user_agent;    // NULL -> "bb_http_client/0.1"
    const char *accept_header; // NULL -> "*/*"
} bb_http_client_cfg_t;

typedef struct {
    int    status_code;        // HTTP status (200, 404, etc.); 0 if transport failed
    size_t body_len;            // bytes written to caller's body buffer
    bool   truncated;           // true if response exceeded body_cap (body still contains the prefix)
} bb_http_client_result_t;

// Fetch `url` with method GET. Caller supplies `body` (capacity `body_cap`)
// to receive the response payload; `out` receives status + length.
// cfg may be NULL to use defaults.
//
// Returns:
//   BB_OK              — transport completed; check out->status_code for HTTP status
//   BB_ERR_INVALID_ARG — NULL url / body / out, or zero body_cap
//   BB_ERR_INVALID_STATE — transport failed (DNS, TLS, socket) after all retries
//   BB_ERR_UNSUPPORTED — platform stub (Arduino today)
bb_err_t bb_http_client_get(const char *url,
                            char *body, size_t body_cap,
                            const bb_http_client_cfg_t *cfg,
                            bb_http_client_result_t *out);

// Callback invoked for each received chunk during bb_http_client_get_stream.
//
// `data` is a transient buffer valid only for the duration of the call;
// `len` is the number of bytes in this chunk (always > 0).
//
// Return BB_OK to continue streaming, any other bb_err_t to abort:
//   BB_ERR_NO_SPACE — signals a clean parser-side "buffer full" abort;
//                     out->truncated will be set to true.
//   Any other error — propagated as-is from bb_http_client_get_stream.
typedef bb_err_t (*bb_http_client_chunk_cb)(void *ctx, const char *data, size_t len);

// Streaming variant of bb_http_client_get. Body is never buffered; instead,
// `cb(ctx, chunk, len)` is called for each received chunk. Only the parser
// state (ctx) lives in memory at the end.
//
// Returns:
//   BB_OK              — transport + all cb() calls completed
//   BB_ERR_INVALID_ARG — NULL url / cb / out
//   BB_ERR_INVALID_STATE — transport failed after all retries
//   BB_ERR_NO_SPACE    — cb returned BB_ERR_NO_SPACE (out->truncated = true)
//   other              — error returned by cb, propagated as-is
//   BB_ERR_UNSUPPORTED — platform stub (Arduino today)
bb_err_t bb_http_client_get_stream(const char *url,
                                   bb_http_client_chunk_cb cb, void *ctx,
                                   const bb_http_client_cfg_t *cfg,
                                   bb_http_client_result_t *out);

#ifdef __cplusplus
}
#endif
