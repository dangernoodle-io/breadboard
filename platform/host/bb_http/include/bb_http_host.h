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
#include "bb_http.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    status;           // last status set (default 200)
    char   content_type[64]; // set by bb_http_resp_set_type or send_json default
    char  *body;             // heap-owned, NUL-terminated; NULL if nothing sent
    size_t body_len;         // length of body (excluding NUL terminator)
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

#ifdef __cplusplus
}
#endif
