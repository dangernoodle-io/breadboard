#pragma once

// HTTP adapter for bb_serialize_json's flush-sink streaming entry point
// (bb_serialize_json_stream_render()). A separate header from
// bb_http_server.h, but that split does NOT achieve header-scoped isolation:
// ESP-IDF's REQUIRES is COMPONENT-scoped, not header-scoped, so
// bb_http_server's CMakeLists.txt REQUIRES-ing bb_serialize_json (to build
// this .c) makes bb_serialize_json a public transitive dependency of every
// bb_http_server consumer, whether or not they #include this header. This
// widening is accepted because bb_serialize_json is dep-light (bb_core/
// bb_num only -- no heap, no ESP-IDF-only deps), so the cost of the
// component-wide REQUIRES is small. The header split still has value: it
// keeps bb_http_server.h itself free of a #include on bb_serialize_json.h,
// so a consumer that never streams a descriptor doesn't pull that API
// surface into scope even though the component dependency is there either
// way.

#include "bb_core.h"
#include "bb_http_server.h"
#include "bb_serialize_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Renders `desc`/`snap` as a single JSON object body, streamed to `req` via
// bb_http_resp_send_chunk() rather than buffered whole in memory first.
// Sets Content-Type: application/json before the first chunk is sent.
//
// ALWAYS finalizes the chunked response with the zero-length terminator
// chunk, even on error -- an unterminated chunked body can hang a strict
// client (same convention as bb_openapi_register.c's openapi_handler).
//
// On a mid-stream send failure (e.g. client disconnect), sets a sticky
// internal abort flag so no further bytes are written/sent, and returns the
// ORIGINAL bb_http_resp_send_chunk() error (not bb_serialize_json's
// synthetic stream-abort code) -- the caller sees the real I/O failure.
// Returns BB_ERR_INVALID_ARG if `req`, `desc`, or `snap` is NULL.
bb_err_t bb_http_serialize_stream(bb_http_request_t *req,
                                   const bb_serialize_desc_t *desc, const void *snap);

// Composed-document counterpart to bb_http_serialize_stream() above -- same
// Content-Type/chunked-finalize/abort-flag wiring, except it streams
// `groups[0..n_groups)` (each its own entries[]/n/shape -- see
// bb_serialize_compose_group_t in bb_serialize_compose.h) via
// bb_serialize_json_stream_compose_render() rather than a single desc/snap
// pair via bb_serialize_json_stream_render(). Returns BB_ERR_INVALID_ARG if
// `req` is NULL, or if `groups` is NULL while `n_groups` is nonzero --
// checked before the Content-Type header is set, same as
// bb_http_serialize_stream() above.
bb_err_t bb_http_serialize_stream_compose(bb_http_request_t *req,
                                           const bb_serialize_compose_group_t *groups, size_t n_groups);

#ifdef __cplusplus
}
#endif
