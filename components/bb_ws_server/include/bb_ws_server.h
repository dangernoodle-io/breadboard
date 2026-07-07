#pragma once

// Portable WebSocket SERVER transport — wraps ESP-IDF httpd's native WS
// support the same way bb_http wraps esp_http_server for REST.
//
// Registration: bb_ws_server_register_endpoint adds a GET route (with
// is_websocket=true on ESP-IDF) to an already-started server.  The registered
// handler receives every incoming DATA frame; control frames (PING/PONG/CLOSE)
// are handled by httpd internally unless handle_ws_control_frames is requested.
//
// Sending from within the handler (sync): bb_ws_server_send_frame.
// Server-push from outside the handler (async): bb_ws_server_broadcast_frame_async
// or the convenience wrapper bb_ws_server_broadcast_all.
//
// Portability: this header includes NO esp_* headers.  All ESP-IDF types are
// hidden behind the opaque bb_http_handle_t / bb_http_request_t handles from
// bb_core.h.  Platform implementations live in platform/espidf/bb_ws_server/
// and platform/host/bb_ws_server/.

#include "bb_core.h"
#include "bb_http.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Frame type enum — mirrors httpd_ws_type_t without the ESP-IDF header
// ---------------------------------------------------------------------------

typedef enum {
    BB_WS_TYPE_CONTINUE = 0x00,
    BB_WS_TYPE_TEXT     = 0x01,
    BB_WS_TYPE_BINARY   = 0x02,
    BB_WS_TYPE_CLOSE    = 0x08,
    BB_WS_TYPE_PING     = 0x09,
    BB_WS_TYPE_PONG     = 0x0A,
} bb_ws_server_frame_type_t;

// ---------------------------------------------------------------------------
// Frame descriptor — portable across all platforms
// ---------------------------------------------------------------------------

typedef struct {
    bool                     final;    // FIN bit; false for fragmented messages
    bb_ws_server_frame_type_t type;
    uint8_t                 *payload;  // caller-owned buffer (recv: pre-alloc; send: const)
    size_t                   len;      // payload byte count
} bb_ws_server_frame_t;

// ---------------------------------------------------------------------------
// Handler type
// ---------------------------------------------------------------------------

// Called for each incoming data frame.  req is the opaque request handle;
// frame carries the received frame.  Return BB_OK to continue, any error to
// close the connection.
typedef bb_err_t (*bb_ws_server_handler_fn)(bb_http_request_t *req,
                                            const bb_ws_server_frame_t *frame);

// ---------------------------------------------------------------------------
// Async broadcast callback — fired after httpd_ws_send_frame_async completes
// ---------------------------------------------------------------------------

// Called by httpd's worker task after async send.  err: BB_OK on success.
// fd: the socket fd the send was targeted at.  ctx: user context.
typedef void (*bb_ws_server_send_cb_t)(bb_err_t err, int fd, void *ctx);

// ---------------------------------------------------------------------------
// Core API
// ---------------------------------------------------------------------------

// Register a WebSocket endpoint at `path` on an already-started server.
// On ESP-IDF registers an httpd_uri_t with is_websocket=true + HTTP_GET.
// On host uses the capture harness (no-op registration, records the handler).
// Returns BB_OK on success.
bb_err_t bb_ws_server_register_endpoint(bb_http_handle_t server,
                                        const char *path,
                                        bb_ws_server_handler_fn handler);

// Receive a frame from within the handler context.  Two-step pattern:
//   1. Call with max_len=0 to probe frame length (populates frame->len).
//   2. Allocate frame->payload of at least frame->len bytes.
//   3. Call again with max_len=frame->len to receive the payload.
// Returns BB_OK on success, BB_ERR_INVALID_ARG on NULL args.
bb_err_t bb_ws_server_recv_frame(bb_http_request_t *req,
                                 bb_ws_server_frame_t *frame,
                                 size_t max_len);

// Send a frame synchronously from within the handler context.
// frame->payload must remain valid for the duration of the call.
// Returns BB_OK on success.
bb_err_t bb_ws_server_send_frame(bb_http_request_t *req,
                                 const bb_ws_server_frame_t *frame);

// Async server-push: queue a send to a specific client fd via httpd_queue_work.
// cb (may be NULL) is called on completion.  ctx is passed through to cb.
// Must NOT be called from within the handler (use send_frame for in-handler
// sends).  Safe to call from any FreeRTOS task or from host test code.
bb_err_t bb_ws_server_broadcast_frame_async(bb_http_handle_t server,
                                            int fd,
                                            const bb_ws_server_frame_t *frame,
                                            bb_ws_server_send_cb_t cb,
                                            void *ctx);

// Broadcast to all currently-active WS clients tracked by httpd.
// Iterates fds 0..BB_WS_SERVER_MAX_FD and sends to each active WS client.
// cb and ctx are forwarded to each individual async send (may be NULL).
bb_err_t bb_ws_server_broadcast_all(bb_http_handle_t server,
                                    const bb_ws_server_frame_t *frame,
                                    bb_ws_server_send_cb_t cb,
                                    void *ctx);

// Return true if fd is an active WebSocket client on server.
// On host, returns the value set by bb_ws_server_host_set_client_active.
bool bb_ws_server_is_client(bb_http_handle_t server, int fd);

// ---------------------------------------------------------------------------
// OpenAPI descriptor registration helper
// ---------------------------------------------------------------------------
// WebSocket routes cannot be natively modelled in OpenAPI 3 (which is
// REST/HTTP-centric).  We register the /ws endpoint as a GET route with a
// custom x-protocol extension so it appears in the spec and the fidelity test
// can find it, rather than silently omitting it.
//
// Usage: bb_ws_server_register_described_endpoint(server, path, handler, &s_ws_route)
// where s_ws_route is a static bb_route_t with method=BB_HTTP_GET and the
// desired summary/tag.  The function calls bb_ws_server_register_endpoint()
// and then bb_http_register_route_descriptor_only() so the descriptor appears
// in the registry for bb_openapi without double-registering a handler.
bb_err_t bb_ws_server_register_described_endpoint(bb_http_handle_t server,
                                                  const char *path,
                                                  bb_ws_server_handler_fn handler,
                                                  const bb_route_t *descriptor);

// Return the socket fd associated with a WebSocket request.
// On ESP-IDF: wraps httpd_req_to_sockfd (returns -1 on error).
// On host: returns the fd set by bb_ws_server_host_set_inject_fd(), or -1 if
// none was set before inject_frame (e.g. handler called without a fake fd).
int bb_ws_server_req_fd(bb_http_request_t *req);

// ---------------------------------------------------------------------------
// Disconnect notification
// ---------------------------------------------------------------------------

// Called once per WS session teardown (socket close, timeout, or server
// shutdown) with the fd that was associated with the closed session.
typedef void (*bb_ws_server_disconnect_cb_t)(int fd, void *ctx);

// Register a callback invoked on WS session teardown. Global — one
// registration per process; a later call replaces the previous one. Pass
// cb=NULL to unregister. ctx is passed through to cb unchanged.
// On ESP-IDF: piggybacks on the same req->sess_ctx/free_ctx hook that backs
// bb_ws_server_open_count(), so no extra per-session bookkeeping is added.
// On host: invoked synchronously by bb_ws_server_host_simulate_disconnect().
void bb_ws_server_set_disconnect_cb(bb_ws_server_disconnect_cb_t cb, void *ctx);

// ---------------------------------------------------------------------------
// Connect notification
// ---------------------------------------------------------------------------

// Called once per WS session establishment (handshake completion), with the
// server handle and the fd of the newly-connected client.
typedef void (*bb_ws_server_connect_cb_t)(bb_http_handle_t server, int fd, void *ctx);

// Register a callback invoked when a new WS session completes its handshake.
// Global — one registration per process; a later call replaces the previous
// one. Pass cb=NULL to unregister. ctx is passed through to cb unchanged.
// On ESP-IDF: fires from the same connect-signal branch (req->sess_ctx ==
// NULL on the first HTTP_GET call for a session) that arms the disconnect
// hook above -- BEFORE any data frame has been received from the client.
// On host: invoked synchronously by bb_ws_server_host_simulate_connect().
void bb_ws_server_set_connect_cb(bb_ws_server_connect_cb_t cb, void *ctx);

// Close a single /ws client session to reclaim heap during a TLS window.
// On ESP-IDF: calls httpd_sess_trigger_close; returns BB_OK on success.
// On host: no-op, returns BB_OK.
bb_err_t bb_ws_server_close_client(bb_http_handle_t server, int fd);

// Return the number of currently-open WebSocket connections across all
// endpoints registered via bb_ws_server_register_endpoint.
// On ESP-IDF: a session counter incremented at WS handshake completion and
// decremented when httpd tears the session down (req->sess_ctx/free_ctx),
// so it stays correct without polling httpd's internal fd table.
// On host: returns a value driven by bb_ws_server_host_simulate_open/close
// (see bb_ws_server_host.h); 0 until simulated.
size_t bb_ws_server_open_count(void);

// Max fd probed by broadcast_all.  On ESP-IDF, LWIP socket fds are offset by
// LWIP_SOCKET_OFFSET = FD_SETSIZE - CONFIG_LWIP_MAX_SOCKETS (typically 64-12=52),
// so the scan upper bound must be FD_SETSIZE (64), NOT CONFIG_HTTPD_MAX_SOCKETS
// (which is the max concurrent connection count, not the max fd number).
// On host, 64 is a safe over-estimate (the host stub only iterates active fds).
#ifdef ESP_PLATFORM
#include <sys/select.h>
#define BB_WS_SERVER_MAX_FD FD_SETSIZE
#else
#ifndef BB_WS_SERVER_MAX_FD
#define BB_WS_SERVER_MAX_FD 64
#endif
#endif

#ifdef __cplusplus
}
#endif
