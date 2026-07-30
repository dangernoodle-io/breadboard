#pragma once

// Single-field {"error": "<msg>"} JSON error body, streamed through the same
// bb_http_serialize_stream()/bb_serialize_desc_t path as every other
// serialized response body (B1-1286) -- factored into its own file (not
// bb_http_serialize_stream.c/.h) so this small, wholly-new addition carries
// its own 100%-coverage floor rather than inheriting bb_http_serialize_
// stream.c's pre-existing, unrelated coverage gaps (touching that file would
// pull its whole-file ratchet in with it -- see scripts/coverage_baseline.py).
//
// This is the {"error":...}-body counterpart to bb_http_send_json_error()
// (bb_http_server.h) -- that fn takes a PREFORMATTED JSON body and is the
// right tool when the body is a static literal; this one takes a raw,
// unescaped message string and streams it through bb_serialize_json's
// escaping, replacing the bb_http_json_obj_stream_t idiom (a 1048-byte stack
// buffer) that used to back this exact shape in three route files'
// send_400/send_500/send_501-style helpers
// (bb_system_routes.c/bb_wifi_http_routes.c/bb_storage_http_routes.c) --
// factoring it here (rather than three near-identical per-file desc/fields/
// snapshot copies) is the Consolidation convention (CLAUDE.md): a shared
// idiom used by more than one caller lands in its central component, not
// re-hand-rolled per call site.

#include "bb_core.h"
#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max bytes (incl. NUL) bb_http_serialize_send_error() copies `msg` into
// before streaming it. A longer `msg` is truncated to fit -- callers with a
// static literal under this bound never observe truncation.
#define BB_HTTP_SERIALIZE_ERROR_MSG_MAX 256

// Sets `status`, then streams {"error":"<msg, JSON-escaped>"} via
// bb_http_serialize_stream(). `msg` may be NULL (renders {"error":""}) or
// non-static (copied into a local snapshot before streaming, so its storage
// need not outlive this call). Returns BB_ERR_INVALID_ARG if `req` is NULL,
// or whatever bb_http_resp_set_status()/bb_http_serialize_stream() itself
// returns on a downstream failure.
bb_err_t bb_http_serialize_send_error(bb_http_request_t *req, int status, const char *msg);

#ifdef __cplusplus
}
#endif
