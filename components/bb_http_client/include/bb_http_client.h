#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"
#include "bb_serialize.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

// Minimum FreeRTOS task stack size (bytes) for any task that calls
// bb_http_client_get, bb_http_client_get_stream, or bb_http_client_post.
// Covers the mbedTLS handshake + cert-bundle parse path. Pass to xTaskCreate
// so the budget stays consistent across consumers (bb_ota_pull,
// bb_ota_check, ...). Override via CONFIG_BB_HTTP_CLIENT_TASK_STACK_SIZE.
#ifndef BB_HTTP_CLIENT_TASK_STACK
#  if defined(CONFIG_BB_HTTP_CLIENT_TASK_STACK_SIZE)
#    define BB_HTTP_CLIENT_TASK_STACK CONFIG_BB_HTTP_CLIENT_TASK_STACK_SIZE
#  else
#    define BB_HTTP_CLIENT_TASK_STACK 8192
#  endif
#endif

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
//
// Per-session health (B1-1041): only the keep-alive SESSION API
// (bb_http_client_session_open/_post/_close) surfaces a health snapshot --
// the one-shot bb_http_client_get/_get_stream/_post calls have no
// persistent object to hang health on, so they are deliberately OUT OF
// SCOPE. A caller that needs health for a one-shot-shaped workflow should
// wrap it in a session instead. See bb_http_client_session_health_fill
// below (near the session API) for the snapshot contract and reporting
// policy.

typedef struct {
    uint32_t timeout_ms;       // 0 -> 10000 (10 s)
    uint8_t  max_attempts;     // 0 -> 3
    uint16_t buffer_size;      // internal client receive buffer; 0 -> 4096
    const char *user_agent;    // NULL -> "bb_http_client/0.1"
    const char *accept_header; // NULL -> "*/*"
    // TLS overrides — all NULL means use ESP public cert bundle (default).
    // Set ca_cert_pem to override CA/server verification (PEM string).
    // Set client_cert_pem + client_key_pem for mutual TLS (both required).
    const char *ca_cert_pem;      // server/CA cert PEM override; NULL => crt_bundle_attach
    const char *client_cert_pem;  // client cert PEM for mutual TLS (optional)
    const char *client_key_pem;   // client private key PEM for mutual TLS (optional)
    // Session keep-alive (used by bb_http_client_session_open only).
    // When true the TCP/TLS connection is reused across posts (default on ESP-IDF).
    // Set false to open+close a fresh connection on every post.
    // Zero-initialised cfg → false (off).  Callers that want keep-alive must
    // set this explicitly or rely on the Kconfig default via session_ensure().
    bool keep_alive;
} bb_http_client_cfg_t;

typedef struct {
    int    status_code;        // HTTP status (200, 404, etc.); 0 if transport failed
    size_t body_len;            // bytes written to caller's body buffer
    bool   truncated;           // true if response exceeded body_cap (body still contains the prefix)
    int    tls_error_code;     // raw mbedtls error (negative int); 0 when none. diagnostic/log-only.
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

// Perform an HTTP POST to `url`, sending `body` (length `body_len`) with the
// given `content_type` (NULL => "application/json"). The response body is
// buffered into caller-supplied `resp` (capacity `resp_cap`); `out` receives
// the status code, response length, and truncation flag.
//
// TLS configuration is taken from cfg->ca_cert_pem / cfg->client_cert_pem /
// cfg->client_key_pem.  All NULL => public cert bundle (default).
//
// cfg may be NULL to use defaults.
//
// Returns:
//   BB_OK              — transport completed; check out->status_code for HTTP status
//   BB_ERR_INVALID_ARG — NULL url / resp / out, or zero resp_cap
//   BB_ERR_INVALID_STATE — transport failed after all retries
//   BB_ERR_UNSUPPORTED — platform stub (Arduino today)
bb_err_t bb_http_client_post(const char *url,
                             const char *body, size_t body_len,
                             const char *content_type,
                             char *resp, size_t resp_cap,
                             const bb_http_client_cfg_t *cfg,
                             bb_http_client_result_t *out);

// ---------------------------------------------------------------------------
// Reusable keep-alive session API
//
// A session wraps a single persistent TCP (or TLS) connection to one host.
// The connection is established lazily on the first bb_http_client_session_post
// call and reused across subsequent posts — no new socket per call.
//
// Typical use: one session per consumer (e.g. bb_ota_pull, bb_ota_check),
// opened once and kept alive for the module lifetime.  On transport error,
// the handle remains valid; the next post attempt will reconnect
// automatically.
//
// cfg may be NULL to use defaults.  TLS fields in cfg (ca_cert_pem,
// client_cert_pem, client_key_pem) are read once at session_open time and
// must remain valid until session_close.
// ---------------------------------------------------------------------------

typedef void *bb_http_client_session_t;

// Open a reusable keep-alive session.  Does not connect immediately; the
// connection is established on the first session_post call.
//
// Returns:
//   BB_OK              — *out is a valid (non-NULL) session handle
//   BB_ERR_INVALID_ARG — NULL url_base or out
//   BB_ERR_NO_SPACE    — allocation failure
bb_err_t bb_http_client_session_open(const bb_http_client_cfg_t *cfg,
                                     const char *url_base,
                                     bb_http_client_session_t *out);

// POST body to url over the reused connection.  The response body is ignored
// (publisher doesn't need it).  out->status_code is set on transport success.
//
// Returns:
//   BB_OK                — transport completed; check out->status_code
//   BB_ERR_INVALID_ARG   — NULL s, url, or out
//   BB_ERR_INVALID_STATE — transport or HTTP error
bb_err_t bb_http_client_session_post(bb_http_client_session_t s,
                                     const char *url,
                                     const char *body, size_t body_len,
                                     const char *content_type,
                                     bb_http_client_result_t *out);

// Set a persistent request header on an open session.  The header is applied
// on every subsequent bb_http_client_session_post call (keep-alive reuse).
// Both `name` and `value` are copied internally; the caller need not keep
// them alive after the call returns.
//
// Call before or between posts; re-calling with the same name overwrites the
// previous value.  Passes through to esp_http_client_set_header on ESP-IDF.
//
// Returns:
//   BB_OK              — header applied
//   BB_ERR_INVALID_ARG — NULL s, name, or value
bb_err_t bb_http_client_session_set_header(bb_http_client_session_t s,
                                            const char *name,
                                            const char *value);

// Close the session and release all resources.  Safe to call with NULL.
void bb_http_client_session_close(bb_http_client_session_t s);

// ---------------------------------------------------------------------------
// Per-session health snapshot (B1-1041) — format-agnostic wire surface,
// mirroring bb_tcp_client_health_snap_t / bb_mqtt_client_health_snap_t /
// bb_meminfo_heap_snap's descriptor+fill pattern. Every numeric field is
// widened to a fixed 64-bit width so bb_serialize_walk()'s
// BB_TYPE_I64/BB_TYPE_U64 cases (which always memcpy a fixed 8 bytes at the
// descriptor offset) never read past a narrower platform type.
//
// SCOPE: session-mode only (see the top-of-file note) -- there is no
// persistent object behind bb_http_client_get/_post to attach health to.
//
// Reporting policy — read this before wiring a consumer FSM off
// bb_http_client_session_health_fill():
//   - session_post() completes the round trip (any HTTP status code)
//     -> connected=true, last_ok_ms stamped
//   - session_post() status_code >= 500 (server error, transport OK)
//     -> ALSO fail_count++
//   - session_post() status_code in [400,499]
//     -> fail_count NOT bumped -- a 4xx is a valid server response, not a
//        transport failure
//   - session_post() transport failure (DNS/TLS/socket, no response)
//     -> connected=false, fail_count++ (status_code ignored)
//   - session_close()
//     -> connected=false, fail_count untouched -- a clean close is not a
//        transport failure
//   - tls_error_code is set whenever a session_post() result carries a
//     non-zero tls_error_code (success or failure path) and otherwise left
//     at its last-reported value -- never reset to 0 on a later success
//
// bb_http_client_session_health_fill() + bb_http_client_session_health_desc
// give a consumer a format-agnostic snapshot of one session's health (the
// bb_tcp_client_health_* / bb_mqtt_client_health_* pattern) -- bb exposes
// RAW metrics; the consumer judges (no verdict here).
// ---------------------------------------------------------------------------

typedef struct {
    bool     connected;
    int64_t  last_ok_ms;
    uint64_t fail_count;
    int64_t  tls_error_code;
} bb_http_client_session_health_snap_t;

extern const bb_serialize_desc_t bb_http_client_session_health_desc;

/**
 * Populates `out` from session `s`'s live health state.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if s is NULL/invalid or out
 *         is NULL.
 */
bb_err_t bb_http_client_session_health_fill(bb_http_client_session_t s,
                                            bb_http_client_session_health_snap_t *out);

#ifdef __cplusplus
}
#endif
