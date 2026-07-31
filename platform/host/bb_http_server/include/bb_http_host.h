#pragma once
// Host-only capture API for bb_http response helpers.
// Used by native tests to intercept handler output without a real HTTP server.
//
// Usage:
//   bb_http_request_t *req;
//   bb_http_host_capture_begin(&req);        // arms a capture slot, returns fake req
//   my_handler(req);                         // handler runs, output captured
//   bb_http_host_capture_t cap;
//   bb_http_host_capture_end(req, &cap);     // disarms slot, fills cap
//   // inspect cap.status, cap.content_type, cap.body
//   bb_http_host_capture_free(&cap);         // frees cap.body heap allocation

#include "bb_core.h"
#include "bb_http_server.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    status;           // last status set (default 200)
    char   content_type[64]; // set by bb_http_resp_set_type or send_json default
    char  *body;             // heap-owned, NUL-terminated; NULL if nothing sent
    size_t body_len;         // length of body (excluding NUL terminator)
    bool   has_acao;         // Access-Control-Allow-Origin was set
    bool   has_acapn;        // Access-Control-Allow-Private-Network was set
} bb_http_host_capture_t;

// Arm a new capture slot. Writes a fake (non-NULL) request cookie to *out_req.
// The same pointer must be passed to the handler and to bb_http_host_capture_end.
// Only one capture slot is active at a time (host tests are single-threaded).
void bb_http_host_capture_begin(bb_http_request_t **out_req);

// Inject a request body into the active capture slot. Must be called after
// bb_http_host_capture_begin and before invoking the handler. The body string
// is referenced (not copied) — it must remain valid until after the handler
// returns. Passing NULL clears any previously injected body.
void bb_http_host_capture_set_req_body(const char *body, int len);

// Inject a single query parameter into the active capture slot. Must be called
// after bb_http_host_capture_begin and before invoking the handler. The strings
// are referenced (not copied) — both must remain valid until after the handler
// returns. Pass NULL key to clear any previously injected param.
// Only one key=value pair is supported at a time.
void bb_http_host_capture_set_query_param(const char *key, const char *val);

// Inject a raw query string for multi-param tests (e.g. "schema&format=json").
// Overrides the single key/val injection when set; both are cleared by
// bb_http_host_capture_begin. The string is referenced (not copied) and must
// remain valid until after the handler returns. Pass NULL to clear.
void bb_http_host_capture_set_query_string(const char *query_string);

// Inject the request URI consulted by bb_http_req_uri. Must be called after
// bb_http_host_capture_begin and before invoking the handler. The string is
// referenced (not copied) and must remain valid until after the handler
// returns. Pass NULL to clear any previously injected URI.
void bb_http_host_capture_set_req_uri(const char *uri);

// Disarm the active capture slot and populate *out with the intercepted response.
// Returns BB_OK on success, BB_ERR_INVALID_ARG on NULL args or if req does not
// match the active slot.
// The caller must call bb_http_host_capture_free(&out) when done.
bb_err_t bb_http_host_capture_end(bb_http_request_t *req,
                                  bb_http_host_capture_t *out);

// Free heap memory owned by a capture (cap->body). Safe to call with NULL body.
void bb_http_host_capture_free(bb_http_host_capture_t *cap);

// Test hook: force bb_http_req_recv to return -1 (simulates read failure).
// Reset to false after the test.
void bb_http_host_force_recv_fail(bool fail);

// Test hook: inject a single request header (name/value) consulted by
// bb_http_req_get_header. Strings are referenced (not copied) — both must
// remain valid until after the handler/assertion runs. Pass NULL name to
// clear (bb_http_req_get_header then returns BB_ERR_NOT_FOUND for any name).
void bb_http_host_set_req_header(const char *name, const char *value);

// Test hook: force bb_http_resp_set_type to return BB_ERR_INVALID_STATE.
// Reset to false after the test.
void bb_http_host_force_set_type_fail(bool fail);

// Test hook: force bb_http_resp_send_chunk to return BB_ERR_NO_SPACE.
// Reset to false after the test.
void bb_http_host_force_send_chunk_fail(bool fail);

// Test hook: force send_chunk to fail only on the terminator call (buf==NULL).
// Reset to false after the test.
void bb_http_host_force_send_chunk_term_fail(bool fail);

// Test hook: total number of bb_http_resp_send_chunk() invocations since the
// active capture slot was armed (bb_http_host_capture_begin), counted on
// EVERY call regardless of forced-failure return -- lets a test distinguish
// "the terminator call was attempted (and happened to fail)" from "the
// terminator was skipped entirely", which the return code alone cannot
// (both a mid-stream fail and a skipped terminator can leave the same
// error propagating out). Reset to 0 by bb_http_host_capture_begin.
size_t bb_http_host_send_chunk_call_count(void);

// Asset wildcard test hooks (mirror the ESP-IDF asset_wildcard_handler logic).
// Register an asset table via bb_http_register_assets first, then call these.

// Serve a single asset into the capture slot.
bb_err_t bb_http_host_serve_asset(bb_http_request_t *req, const bb_http_asset_t *asset);

// Look up `uri` in the registered asset table and serve it, or set 404.
// Handles "/" → "/index.html" convention and query-string stripping.
bb_err_t bb_http_host_asset_wildcard(bb_http_request_t *req, const char *uri);

// Reset the stored asset table (call in teardown after wildcard tests).
void bb_http_host_reset_assets(void);

// ---------------------------------------------------------------------------
// Provisioning-gate dispatch mirrors (B1-1279 PR3). Each mirrors one of the
// three ESP-IDF insertion points in bb_http.c so host tests can exercise the
// actual gate-checking logic at each site, not just the underlying pure
// bb_http_prov_gate_allow() predicate. See bb_http.c for the device-side
// counterparts (api_dispatch_handler, bb_shim_handler, asset_wildcard_handler
// — the asset counterpart is bb_http_host_asset_wildcard() above, which
// gates internally).
// ---------------------------------------------------------------------------

// Host mirror of api_dispatch_handler: gate, then dispatch through the
// shared bb_dispatch_api table (register routes first via
// bb_http_register_route with an "/api/..." path).
bb_err_t bb_http_host_api_dispatch(bb_http_request_t *req,
                                   bb_http_method_t method,
                                   const char *uri);

// Host mirror of bb_shim_handler: gate, then dispatch a non-/api route
// previously registered via bb_http_register_route. Denied and
// never-registered both resolve to 404, matching the device handler's
// deny-looks-like-404 contract.
bb_err_t bb_http_host_dispatch_route(bb_http_request_t *req,
                                     bb_http_method_t method,
                                     const char *uri);

// Reset the non-/api route registry populated by bb_http_register_route
// (call in teardown after bb_http_host_dispatch_route tests).
void bb_http_host_reset_routes(void);

// Host mirror of method_not_allowed_err_handler: gate on `method` when
// `mapped` is true (mirrors bb_http_method_from_httpd() succeeding); when
// `mapped` is false (mirrors an unmappable method, e.g. HEAD), fails closed
// with 404 while provisioning is active instead of falling through to the
// registration check. Otherwise resolves 404 (unregistered) vs 405
// (registered, wrong method) exactly like the device handler.
bb_err_t bb_http_host_method_not_allowed(bb_http_request_t *req, bool mapped,
                                         bb_http_method_t method, const char *uri);

#ifdef __cplusplus
}
#endif
