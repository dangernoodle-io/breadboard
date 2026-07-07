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

// Asset wildcard test hooks (mirror the ESP-IDF asset_wildcard_handler logic).
// Register an asset table via bb_http_register_assets first, then call these.

// Serve a single asset into the capture slot.
bb_err_t bb_http_host_serve_asset(bb_http_request_t *req, const bb_http_asset_t *asset);

// Look up `uri` in the registered asset table and serve it, or set 404.
// Handles "/" → "/index.html" convention and query-string stripping.
bb_err_t bb_http_host_asset_wildcard(bb_http_request_t *req, const char *uri);

// Reset the stored asset table (call in teardown after wildcard tests).
void bb_http_host_reset_assets(void);

#ifdef __cplusplus
}
#endif
