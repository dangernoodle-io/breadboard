#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// bb_http_client — portable outbound HTTP GET wrapper.
//
// Designed for one-shot release-manifest / status fetches where the caller
// owns the response buffer. Streaming and large responses are out of scope;
// for those, callers should use the platform client directly (ESP-IDF) or
// file a feature request.
//
// Implementations:
//   - ESP-IDF: esp_http_client_perform with TLS via the ESP cert bundle.
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

#ifdef __cplusplus
}
#endif
