#pragma once

// bb_http_body — portable request-body read helper.
//
// Extracts the malloc→recv→NUL-terminate idiom shared by POST/PATCH/DELETE
// handlers into a single reusable function.

#include "bb_core.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read the request body into a freshly malloc'd, NUL-terminated buffer.
//
// Semantics:
//   - reads body length via bb_http_req_body_len(req)
//   - returns BB_ERR_INVALID_ARG if body length <= 0
//   - returns BB_ERR_NO_SPACE   if body length > max_bytes
//   - malloc(len+1); returns BB_ERR_NO_SPACE on OOM
//   - calls bb_http_req_recv; on failure frees and returns BB_ERR_INVALID_ARG
//   - NUL-terminates at *out_len bytes; sets *out_buf and *out_len
//   - returns BB_OK on success; caller owns *out_buf and must free() it
//
// The helper does NOT send HTTP error responses — the caller keeps its existing
// error-status mapping based on the returned bb_err_t.
bb_err_t bb_http_req_recv_body_alloc(bb_http_request_t *req,
                                     size_t             max_bytes,
                                     char             **out_buf,
                                     int               *out_len);

// ---------------------------------------------------------------------------
// Test hook — compiled only when BB_HTTP_BODY_TESTING is defined.
// Allows unit tests to swap the internal malloc to exercise the OOM branch.
// ---------------------------------------------------------------------------
#ifdef BB_HTTP_BODY_TESTING
void bb_http_body_set_malloc(void *(*fn)(size_t));
#endif

#ifdef __cplusplus
}
#endif
