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

// Read the request body into a CALLER-OWNED, fixed-size stack buffer,
// NUL-terminated. Extracts the validate-cap -> fixed-stack-buffer -> recv
// idiom hand-rolled independently by bb_http_section_dispatch.c,
// bb_wifi_http_routes.c's wifi_patch_handler, and
// bb_storage_http_routes.c's factory_reset_handler (bb_http_section PR
// review, MEDIUM finding) -- every one of those hand-rolled copies checked
// `body_len > buf_cap` (allowing a body of EXACTLY buf_cap bytes) but then
// called bb_http_req_recv(req, buf, sizeof(buf) - 1), silently truncating
// that exactly-at-cap body by one byte.
//
// Cap semantics (DELIBERATE, to make the off-by-one unrepresentable at the
// call site): `buf_cap` is the TOTAL size of `buf` INCLUDING the NUL
// terminator slot -- i.e. always pass sizeof(buf) for a `char buf[N]`
// stack array, exactly like snprintf()'s own `size` parameter. The maximum
// body this can hold is therefore `buf_cap - 1` bytes; a body of exactly
// that size is accepted and NOT truncated.
//
// Semantics:
//   - requires buf_cap >= 1; returns BB_ERR_INVALID_ARG otherwise
//   - reads body length via bb_http_req_body_len(req)
//   - returns BB_ERR_INVALID_ARG if body length <= 0 or > (buf_cap - 1)
//   - calls bb_http_req_recv(req, buf, buf_cap - 1); on failure (n < 0)
//     returns BB_ERR_INVALID_ARG (buf's contents are then undefined)
//   - NUL-terminates buf[n] on success; sets *out_len = n; returns BB_OK
//
// The helper does NOT send HTTP error responses — the caller keeps its
// existing error-status mapping based on the returned bb_err_t (see
// bb_http_send_json_error() in bb_http_server.h for the matching error-body
// helper).
bb_err_t bb_http_req_recv_body_stack(bb_http_request_t *req,
                                     char              *buf,
                                     size_t             buf_cap,
                                     size_t            *out_len);

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
