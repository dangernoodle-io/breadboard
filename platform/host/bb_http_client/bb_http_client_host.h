#pragma once
// Host-only test hooks for bb_http_client. Tests register a canned response;
// subsequent bb_http_client_get, bb_http_client_get_stream, and
// bb_http_client_post calls return it.
// For _get_stream, the mock body is replayed in ~256-byte chunks.
#include <stddef.h>
#include <stdbool.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Set the mock response. `body` is borrowed (typically a string literal);
// caller must keep it alive for the duration of any bb_http_client_* calls.
void bb_http_client_set_mock_response(const char *body, size_t body_len,
                                      int status_code);

// Force the next bb_http_client_get / bb_http_client_get_stream /
// bb_http_client_post call to return a transport error.
void bb_http_client_set_mock_transport_error(bb_err_t err);

// Reset to "no mock set" — subsequent calls return BB_ERR_INVALID_STATE.
// Also clears the last_post record.
void bb_http_client_clear_mock(void);

// -------------------------------------------------------------------------
// POST capture — populated by each bb_http_client_post call on the host.
// Strings point into the caller-supplied buffers; safe to read until the
// next call or clear_mock.  method is always "POST".
// -------------------------------------------------------------------------
typedef struct {
    bool        called;
    const char *method;        // "POST"
    const char *url;
    const char *body;          // pointer into caller's body arg (may be NULL)
    size_t      body_len;
    const char *content_type;  // effective content-type sent (never NULL after a call)
    bool        has_client_cert;  // true if cfg->client_cert_pem was non-NULL
    bool        has_client_key;   // true if cfg->client_key_pem was non-NULL
    bool        has_ca_cert;      // true if cfg->ca_cert_pem was non-NULL
} bb_http_client_post_record_t;

// Return the capture record from the last bb_http_client_post call.
// The record is reset by bb_http_client_clear_mock().
bb_http_client_post_record_t bb_http_client_get_last_post(void);

#ifdef __cplusplus
}
#endif
