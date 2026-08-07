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
//
// Trap: this fn sets Content-Type itself before streaming, so a caller's own
// bb_http_resp_set_type() pre-check LOOKS like a removable duplicate. It is
// NOT removable if the caller's failure path has side effects beyond the
// HTTP response -- e.g. a handler that arms a restart/reboot after this
// call returns. This fn's own return value alone does not stop such a
// caller from running that side effect: if the caller ignores this fn's
// return (as a fire-and-forget restart handler may), a Content-Type-set
// failure inside this call would be swallowed and the side effect would
// still fire, having sent ZERO response bytes to the client. Keep the
// caller's own pre-check in that case (see bb_system_routes.c's
// reboot_handler for the precedent).
bb_err_t bb_http_serialize_stream(bb_http_request_t *req,
                                   const bb_serialize_desc_t *desc, const void *snap);

// Config for bb_http_serialize_stream_compose() (B1-1438) -- collapses the
// former bb_http_serialize_stream_compose()/bb_http_serialize_stream_compose_ex()
// variant-ladder pair into one entry point, same idiom as
// bb_serialize_json_stream_compose_render_cfg_t (bb_serialize_json.h,
// B1-1437). `groups` is legitimately NULL when `n_groups == 0`.
// `f64_shortest == false` reproduces the pre-collapse
// bb_http_serialize_stream_compose()'s fixed behavior -- see
// bb_serialize_json_ctx_t.f64_shortest's doc comment (bb_serialize_json.h)
// for the fixed-decimal (false) vs shortest-round-trippable (true) contract
// (B1-1102).
typedef struct {
    const bb_serialize_compose_group_t *groups;
    size_t                              n_groups;
    bool                                f64_shortest;
} bb_http_serialize_stream_compose_cfg_t;

// Composed-document counterpart to bb_http_serialize_stream() above -- same
// Content-Type/chunked-finalize/abort-flag wiring, except it streams
// `cfg->groups[0..cfg->n_groups)` (each its own entries[]/n/shape -- see
// bb_serialize_compose_group_t in bb_serialize_compose.h) via
// bb_serialize_json_stream_compose_render() rather than a single desc/snap
// pair via bb_serialize_json_stream_render(). Returns BB_ERR_INVALID_ARG if
// `req` or `cfg` is NULL, or if `cfg->groups` is NULL while `cfg->n_groups`
// is nonzero -- checked before the Content-Type header is set, same as
// bb_http_serialize_stream() above.
bb_err_t bb_http_serialize_stream_compose(bb_http_request_t *req,
                                           const bb_http_serialize_stream_compose_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
