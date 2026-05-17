#pragma once
// Host-only test hooks for bb_http_client. Tests register a canned response;
// subsequent bb_http_client_get and bb_http_client_get_stream calls return it.
// For _get_stream, the mock body is replayed in ~256-byte chunks.
#include <stddef.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Set the mock response. `body` is borrowed (typically a string literal);
// caller must keep it alive for the duration of any bb_http_client_* calls.
void bb_http_client_set_mock_response(const char *body, size_t body_len,
                                      int status_code);

// Force the next bb_http_client_get / bb_http_client_get_stream call to
// return a transport error (before any cb() calls).
void bb_http_client_set_mock_transport_error(bb_err_t err);

// Reset to "no mock set" — subsequent calls return BB_ERR_INVALID_STATE.
void bb_http_client_clear_mock(void);

#ifdef __cplusplus
}
#endif
