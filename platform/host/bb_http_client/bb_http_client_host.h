#pragma once
// Host-only test hooks for bb_http_client. Tests register a canned response;
// subsequent bb_http_client_get calls return it.
#include <stddef.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Set the mock response. `body` is borrowed (typically a string literal);
// caller must keep it alive for the duration of any bb_http_client_get calls.
void bb_http_client_set_mock_response(const char *body, size_t body_len,
                                      int status_code);

// Force the next bb_http_client_get call to return a transport error.
void bb_http_client_set_mock_transport_error(bb_err_t err);

// Reset to "no mock set" — subsequent get calls return BB_ERR_INVALID_STATE.
void bb_http_client_clear_mock(void);

#ifdef __cplusplus
}
#endif
