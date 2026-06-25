#pragma once
// Host-only capture API for bb_websocket — mirrors bb_http_host.h.
// Used by native tests to exercise all bb_websocket branches without an
// actual HTTP server or WebSocket connection.
//
// Usage:
//   // Register first (captures the handler fn internally):
//   bb_websocket_register_endpoint(NULL, "/ws", my_handler);
//
//   // Inject an incoming frame and capture any reply:
//   bb_websocket_host_capture_t cap = {0};
//   bb_http_request_t *req = NULL;
//   bb_websocket_host_capture_begin(&req);
//   bb_websocket_frame_t in_frame = { .final=true, .type=BB_WS_TYPE_TEXT,
//                                     .payload=(uint8_t*)"hi", .len=2 };
//   bb_websocket_host_inject_frame(req, &in_frame);
//   bb_websocket_host_capture_sent_frame(&cap);
//   // inspect cap.frame, cap.err
//   bb_websocket_host_capture_free(&cap);

#include "bb_websocket.h"
#include "bb_http.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Captured outgoing frame.
typedef struct {
    bb_websocket_frame_type_t type;
    bool                      final;
    uint8_t                  *payload;  // heap copy; free via bb_websocket_host_capture_free
    size_t                    len;
    bb_err_t                  err;      // result of the send_frame call
} bb_websocket_host_capture_t;

// ----- Registration -------------------------------------------------------

// bb_websocket_register_endpoint on host is a no-op for the httpd side;
// it stores the handler internally so inject_frame can call it.

// ----- Request cookie -----------------------------------------------------

// Arm a new fake request slot.  Writes a non-NULL stable pointer to *out_req.
// Call before bb_websocket_host_inject_frame.
void bb_websocket_host_capture_begin(bb_http_request_t **out_req);

// ----- Frame injection ----------------------------------------------------

// Deliver in_frame to the registered handler and capture any reply written
// via bb_websocket_send_frame.  req must be the pointer from capture_begin.
// Returns the handler's return value.
bb_err_t bb_websocket_host_inject_frame(bb_http_request_t *req,
                                        const bb_websocket_frame_t *in_frame);

// ----- Sent-frame capture -------------------------------------------------

// Retrieve the last frame passed to bb_websocket_send_frame since the last
// capture_begin.  Ownership of out->payload is transferred to the caller;
// call bb_websocket_host_capture_free to release it.
void bb_websocket_host_capture_sent_frame(bb_websocket_host_capture_t *out);

// Free payload owned by a capture.  Safe to call with NULL payload.
void bb_websocket_host_capture_free(bb_websocket_host_capture_t *cap);

// ----- Reset --------------------------------------------------------------

// Clear all capture state (registered handler, pending frames, sent frame).
// Call in teardown between tests.
void bb_websocket_host_reset_captures(void);

// ----- Async send capture -------------------------------------------------

typedef struct {
    bb_websocket_frame_type_t type;
    bool                      final;
    uint8_t                  *payload;   // heap copy
    size_t                    len;
    int                       fd;
    bb_err_t                  err;
} bb_websocket_host_async_capture_t;

// Maximum number of async sends captured per test.
#define BB_WEBSOCKET_HOST_ASYNC_CAP 8

// Retrieve the number of async sends captured since last reset.
int bb_websocket_host_async_count(void);

// Retrieve async capture at index i (0-based).  Returns NULL if out of range.
const bb_websocket_host_async_capture_t *bb_websocket_host_async_at(int i);

// Free all async captures.
void bb_websocket_host_async_reset(void);

// ----- Client-active stub -------------------------------------------------

// Set whether fd is reported as an active WS client (used by is_client and
// broadcast_all to iterate).  Pass fd=-1 to clear all.
void bb_websocket_host_set_client_active(int fd, bool active);

// Set the fd that bb_websocket_req_fd() returns during bb_websocket_host_inject_frame.
// Call before inject_frame to simulate a per-connection subscription update.
// Pass -1 to clear (default).
void bb_websocket_host_set_inject_fd(int fd);

// ----- Force-fail hooks ---------------------------------------------------

// Force bb_websocket_register_endpoint to return BB_ERR_INVALID_STATE.
void bb_websocket_host_force_register_fail(bool fail);

// Force bb_websocket_recv_frame to return BB_ERR_INVALID_STATE.
void bb_websocket_host_force_recv_fail(bool fail);

// Force bb_websocket_send_frame to return BB_ERR_NO_SPACE.
void bb_websocket_host_force_send_fail(bool fail);

// Force bb_websocket_broadcast_frame_async malloc to fail.
void bb_websocket_host_force_async_alloc_fail(bool fail);

#ifdef __cplusplus
}
#endif
