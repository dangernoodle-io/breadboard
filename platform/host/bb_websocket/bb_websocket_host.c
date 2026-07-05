// Host stub for bb_websocket.  Provides a capture harness for unit tests.
// Mirrors bb_http_host.c in structure: fake request cookie, frame capture,
// force-fail test hooks, and async capture tables.

#include "bb_websocket.h"
#include "bb_websocket_host.h"
#include "bb_str.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

// Registered handler (from bb_websocket_register_endpoint)
static bb_websocket_handler_fn s_handler = NULL;
static char                    s_registered_path[128];

// Fake request cookie (stable address, same pattern as bb_http_host)
static int s_capture_cookie;
static bb_http_request_t *s_active_req = NULL;

// Last frame injected (recv probe/data)
static bb_websocket_frame_t s_injected_frame;
static uint8_t             *s_injected_payload = NULL;  // heap copy

// Last frame sent (from bb_websocket_send_frame)
typedef struct {
    bool                      active;
    bb_websocket_frame_type_t type;
    bool                      final;
    uint8_t                  *payload;  // heap copy
    size_t                    len;
    bb_err_t                  err;
} sent_frame_slot_t;
static sent_frame_slot_t s_sent;

// Async capture table
static bb_websocket_host_async_capture_t s_async[BB_WEBSOCKET_HOST_ASYNC_CAP];
static int s_async_count = 0;

// Client active flags (fd index 0..BB_WEBSOCKET_MAX_FD-1)
static bool s_client_active[BB_WEBSOCKET_MAX_FD];

// Open-connection counter, driven by bb_websocket_host_simulate_open/close
// (the espidf connect/disconnect hooks have no host-testable seam — they
// depend on httpd_req_t->sess_ctx/free_ctx, so this is a plain counter).
static size_t s_open_count = 0;

// Disconnect callback registration, mirrors the espidf backend.
static bb_websocket_disconnect_cb_t s_disconnect_cb  = NULL;
static void                        *s_disconnect_ctx = NULL;

// Connect callback registration, mirrors the espidf backend.
static bb_websocket_connect_cb_t s_connect_cb  = NULL;
static void                     *s_connect_ctx = NULL;

// Force-fail flags
static bool s_force_register_fail  = false;
static bool s_force_recv_fail      = false;
static bool s_force_send_fail      = false;
static bool s_force_async_alloc_fail = false;

// Inject fd — set before inject_frame to simulate a per-client fd
static int s_inject_fd = -1;

// ---------------------------------------------------------------------------
// Force-fail hooks
// ---------------------------------------------------------------------------

void bb_websocket_host_force_register_fail(bool fail)
{
    s_force_register_fail = fail;
}

void bb_websocket_host_force_recv_fail(bool fail)
{
    s_force_recv_fail = fail;
}

void bb_websocket_host_force_send_fail(bool fail)
{
    s_force_send_fail = fail;
}

void bb_websocket_host_force_async_alloc_fail(bool fail)
{
    s_force_async_alloc_fail = fail;
}

// ---------------------------------------------------------------------------
// bb_websocket_req_fd (host)
// ---------------------------------------------------------------------------

int bb_websocket_req_fd(bb_http_request_t *req)
{
    (void)req;
    return s_inject_fd;
}

// ---------------------------------------------------------------------------
// bb_websocket_host_set_inject_fd
// ---------------------------------------------------------------------------

void bb_websocket_host_set_inject_fd(int fd)
{
    s_inject_fd = fd;
}

// ---------------------------------------------------------------------------
// bb_websocket_register_endpoint (host)
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_register_endpoint(bb_http_handle_t server,
                                        const char *path,
                                        bb_websocket_handler_fn handler)
{
    (void)server;
    if (s_force_register_fail) {
        return BB_ERR_INVALID_STATE;
    }
    if (!path || !handler) {
        return BB_ERR_INVALID_ARG;
    }
    s_handler = handler;
    bb_strlcpy(s_registered_path, path, sizeof(s_registered_path));
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_websocket_recv_frame (host)
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_recv_frame(bb_http_request_t *req,
                                 bb_websocket_frame_t *frame,
                                 size_t max_len)
{
    if (!req || !frame) {
        return BB_ERR_INVALID_ARG;
    }
    if (s_force_recv_fail) {
        return BB_ERR_INVALID_STATE;
    }

    // Copy injected frame metadata into the caller's frame.
    frame->type  = s_injected_frame.type;
    frame->final = s_injected_frame.final;
    frame->len   = s_injected_frame.len;

    if (max_len == 0) {
        // Probe call — just return length.
        frame->payload = NULL;
        return BB_OK;
    }

    // Data call — copy payload if buffer provided.
    if (frame->payload && s_injected_payload && s_injected_frame.len > 0) {
        size_t copy = s_injected_frame.len < max_len ? s_injected_frame.len : max_len;
        memcpy(frame->payload, s_injected_payload, copy);
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_websocket_send_frame (host)
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_send_frame(bb_http_request_t *req,
                                 const bb_websocket_frame_t *frame)
{
    if (!req || !frame) {
        return BB_ERR_INVALID_ARG;
    }
    if (s_force_send_fail) {
        s_sent.err = BB_ERR_NO_SPACE;
        return BB_ERR_NO_SPACE;
    }

    // Store a heap copy of the sent frame for test inspection.
    free(s_sent.payload);
    s_sent.payload = NULL;
    s_sent.len     = 0;

    if (frame->len > 0 && frame->payload) {
        s_sent.payload = malloc(frame->len);
        if (s_sent.payload) {
            memcpy(s_sent.payload, frame->payload, frame->len);
            s_sent.len = frame->len;
        }
    }
    s_sent.type   = frame->type;
    s_sent.final  = frame->final;
    s_sent.err    = BB_OK;
    s_sent.active = true;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_websocket_is_client (host)
// ---------------------------------------------------------------------------

bool bb_websocket_is_client(bb_http_handle_t server, int fd)
{
    (void)server;
    if (fd < 0 || fd >= BB_WEBSOCKET_MAX_FD) {
        return false;
    }
    return s_client_active[fd];
}

// ---------------------------------------------------------------------------
// bb_websocket_broadcast_frame_async (host)
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_broadcast_frame_async(bb_http_handle_t server,
                                            int fd,
                                            const bb_websocket_frame_t *frame,
                                            bb_websocket_send_cb_t cb,
                                            void *ctx)
{
    (void)server;
    if (!frame) {
        return BB_ERR_INVALID_ARG;
    }
    if (s_force_async_alloc_fail) {
        if (cb) {
            cb(BB_ERR_NO_SPACE, fd, ctx);
        }
        return BB_ERR_NO_SPACE;
    }
    if (s_async_count >= BB_WEBSOCKET_HOST_ASYNC_CAP) {
        return BB_ERR_NO_SPACE;
    }

    bb_websocket_host_async_capture_t *a = &s_async[s_async_count];
    a->type  = frame->type;
    a->final = frame->final;
    a->fd    = fd;
    a->len   = frame->len;
    a->payload = NULL;
    if (frame->len > 0 && frame->payload) {
        a->payload = malloc(frame->len);
        if (a->payload) {
            memcpy(a->payload, frame->payload, frame->len);
        }
    }
    a->err = BB_OK;
    s_async_count++;

    // Simulate immediate completion (host is synchronous).
    if (cb) {
        cb(BB_OK, fd, ctx);
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_websocket_broadcast_all (host)
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_broadcast_all(bb_http_handle_t server,
                                    const bb_websocket_frame_t *frame,
                                    bb_websocket_send_cb_t cb,
                                    void *ctx)
{
    if (!frame) {
        return BB_ERR_INVALID_ARG;
    }
    bb_err_t last_err = BB_OK;
    for (int fd = 0; fd < BB_WEBSOCKET_MAX_FD; fd++) {
        if (bb_websocket_is_client(server, fd)) {
            bb_err_t err = bb_websocket_broadcast_frame_async(server, fd, frame, cb, ctx);
            if (err != BB_OK) {
                last_err = err;
            }
        }
    }
    return last_err;
}

// ---------------------------------------------------------------------------
// bb_websocket_register_described_endpoint (host)
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_register_described_endpoint(bb_http_handle_t server,
                                                  const char *path,
                                                  bb_websocket_handler_fn handler,
                                                  const bb_route_t *descriptor)
{
    if (!path || !handler || !descriptor) {
        return BB_ERR_INVALID_ARG;
    }
    bb_err_t err = bb_websocket_register_endpoint(server, path, handler);
    if (err != BB_OK) {
        return err;
    }
    return bb_http_register_route_descriptor_only(descriptor);
}

// ---------------------------------------------------------------------------
// Capture API
// ---------------------------------------------------------------------------

void bb_websocket_host_capture_begin(bb_http_request_t **out_req)
{
    s_active_req = (bb_http_request_t *)&s_capture_cookie;

    // Clear injected payload from previous test.
    free(s_injected_payload);
    s_injected_payload = NULL;
    memset(&s_injected_frame, 0, sizeof(s_injected_frame));

    // Clear sent frame.
    free(s_sent.payload);
    memset(&s_sent, 0, sizeof(s_sent));

    if (out_req) {
        *out_req = s_active_req;
    }
}

bb_err_t bb_websocket_host_inject_frame(bb_http_request_t *req,
                                        const bb_websocket_frame_t *in_frame)
{
    if (!req || !in_frame || !s_handler) {
        return BB_ERR_INVALID_STATE;
    }

    // Copy frame into injected slot so recv_frame can serve it.
    s_injected_frame.type  = in_frame->type;
    s_injected_frame.final = in_frame->final;
    s_injected_frame.len   = in_frame->len;

    free(s_injected_payload);
    s_injected_payload = NULL;
    if (in_frame->len > 0 && in_frame->payload) {
        s_injected_payload = malloc(in_frame->len);
        if (s_injected_payload) {
            memcpy(s_injected_payload, in_frame->payload, in_frame->len);
        }
    }

    return s_handler(req, in_frame);
}

void bb_websocket_host_capture_sent_frame(bb_websocket_host_capture_t *out)
{
    if (!out) {
        return;
    }
    out->type    = s_sent.type;
    out->final   = s_sent.final;
    out->len     = s_sent.len;
    out->err     = s_sent.err;
    out->payload = s_sent.payload;  // transfer ownership

    // Disarm so a second call returns empty.
    s_sent.payload = NULL;
    s_sent.len     = 0;
    s_sent.active  = false;
}

void bb_websocket_host_capture_free(bb_websocket_host_capture_t *cap)
{
    if (!cap) {
        return;
    }
    free(cap->payload);
    cap->payload = NULL;
    cap->len     = 0;
}

void bb_websocket_host_reset_captures(void)
{
    s_handler = NULL;
    memset(s_registered_path, 0, sizeof(s_registered_path));
    s_active_req = NULL;

    free(s_injected_payload);
    s_injected_payload = NULL;
    memset(&s_injected_frame, 0, sizeof(s_injected_frame));

    free(s_sent.payload);
    memset(&s_sent, 0, sizeof(s_sent));

    bb_websocket_host_async_reset();

    memset(s_client_active, 0, sizeof(s_client_active));
    s_open_count = 0;

    s_disconnect_cb  = NULL;
    s_disconnect_ctx = NULL;

    s_connect_cb  = NULL;
    s_connect_ctx = NULL;

    s_force_register_fail    = false;
    s_force_recv_fail        = false;
    s_force_send_fail        = false;
    s_force_async_alloc_fail = false;

    s_inject_fd = -1;
}

// ---------------------------------------------------------------------------
// Async capture accessors
// ---------------------------------------------------------------------------

int bb_websocket_host_async_count(void)
{
    return s_async_count;
}

const bb_websocket_host_async_capture_t *bb_websocket_host_async_at(int i)
{
    if (i < 0 || i >= s_async_count) {
        return NULL;
    }
    return &s_async[i];
}

void bb_websocket_host_async_reset(void)
{
    for (int i = 0; i < s_async_count; i++) {
        free(s_async[i].payload);
        s_async[i].payload = NULL;
    }
    s_async_count = 0;
}

// ---------------------------------------------------------------------------
// Client active stub
// ---------------------------------------------------------------------------

void bb_websocket_host_set_client_active(int fd, bool active)
{
    if (fd < 0) {
        memset(s_client_active, 0, sizeof(s_client_active));
        return;
    }
    if (fd < BB_WEBSOCKET_MAX_FD) {
        s_client_active[fd] = active;
    }
}

// ---------------------------------------------------------------------------
// bb_websocket_close_client (host — no-op)
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_close_client(bb_http_handle_t server, int fd)
{
    (void)server; (void)fd;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_websocket_open_count (host)
// ---------------------------------------------------------------------------

size_t bb_websocket_open_count(void)
{
    return s_open_count;
}

void bb_websocket_host_simulate_open(void)
{
    s_open_count++;
}

void bb_websocket_host_simulate_close(void)
{
    if (s_open_count > 0) {
        s_open_count--;
    }
}

// ---------------------------------------------------------------------------
// bb_websocket_set_disconnect_cb / simulate_disconnect (host)
// ---------------------------------------------------------------------------

void bb_websocket_set_disconnect_cb(bb_websocket_disconnect_cb_t cb, void *ctx)
{
    s_disconnect_cb  = cb;
    s_disconnect_ctx = ctx;
}

void bb_websocket_host_simulate_disconnect(int fd)
{
    if (s_disconnect_cb) {
        s_disconnect_cb(fd, s_disconnect_ctx);
    }
}

// ---------------------------------------------------------------------------
// bb_websocket_set_connect_cb / simulate_connect (host)
// ---------------------------------------------------------------------------

void bb_websocket_set_connect_cb(bb_websocket_connect_cb_t cb, void *ctx)
{
    s_connect_cb  = cb;
    s_connect_ctx = ctx;
}

void bb_websocket_host_simulate_connect(bb_http_handle_t server, int fd)
{
    if (s_connect_cb) {
        s_connect_cb(server, fd, s_connect_ctx);
    }
}
