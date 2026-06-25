// ESP-IDF implementation of bb_websocket.
// Wraps httpd_ws_* APIs (CONFIG_HTTPD_WS_SUPPORT, default y).
// bb_http_request_t is cast to httpd_req_t *; bb_http_handle_t to httpd_handle_t.
// Async broadcast uses httpd_queue_work + httpd_ws_send_frame_async.
//
// Portability guard: the entire file is compiled only when ESP_PLATFORM is
// defined (ensured by idf_component_register in CMakeLists.txt).

#ifdef ESP_PLATFORM
#include "sdkconfig.h"

#if CONFIG_HTTPD_WS_SUPPORT

#include "bb_websocket.h"
#include "bb_log.h"

#include "esp_http_server.h"
#include "esp_err.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "bb_ws";

// ---------------------------------------------------------------------------
// Internal shim — bridges httpd_uri_t handler to bb_websocket_handler_fn
// ---------------------------------------------------------------------------

// Per-endpoint user_ctx stored on the httpd_uri_t.
typedef struct {
    bb_websocket_handler_fn handler;
} ws_endpoint_ctx_t;

static esp_err_t ws_shim_handler(httpd_req_t *req)
{
    ws_endpoint_ctx_t *ctx = (ws_endpoint_ctx_t *)req->user_ctx;
    if (!ctx || !ctx->handler) {
        return ESP_FAIL;
    }

    // httpd handles the HTTP→WS upgrade internally on the first GET that
    // triggers is_websocket.  Subsequent calls carry actual frame data.
    if (req->method == HTTP_GET) {
        // Upgrade request — httpd has completed the handshake at this point;
        // nothing to do in the handler.
        return ESP_OK;
    }

    // Receive the frame header first (max_len=0 probes length).
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK) {
        bb_log_w(TAG, "ws recv probe failed: %d", err);
        return err;
    }

    // Allocate payload buffer and receive full frame.
    uint8_t *buf = NULL;
    if (ws_pkt.len > 0) {
        buf = calloc(1, ws_pkt.len + 1);
        if (!buf) {
            bb_log_e(TAG, "ws recv alloc failed (%zu bytes)", ws_pkt.len);
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (err != ESP_OK) {
            bb_log_w(TAG, "ws recv frame failed: %d", err);
            free(buf);
            return err;
        }
    }

    // Build portable frame descriptor and call the user handler.
    bb_websocket_frame_t frame = {
        .final   = ws_pkt.final,
        .type    = (bb_websocket_frame_type_t)ws_pkt.type,
        .payload = ws_pkt.payload,
        .len     = ws_pkt.len,
    };

    bb_err_t rc = ctx->handler((bb_http_request_t *)req, &frame);

    free(buf);
    return (rc == BB_OK) ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------------------
// bb_websocket_register_endpoint
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_register_endpoint(bb_http_handle_t server,
                                        const char *path,
                                        bb_websocket_handler_fn handler)
{
    if (!server || !path || !handler) {
        return BB_ERR_INVALID_ARG;
    }

    httpd_handle_t h = (httpd_handle_t)server;

    // ctx is never freed (httpd lifetime == server lifetime); acceptable for
    // the endpoint count (small; same pattern as bb_event_routes SSE ctx).
    ws_endpoint_ctx_t *ctx = malloc(sizeof(ws_endpoint_ctx_t));
    if (!ctx) {
        return BB_ERR_NO_SPACE;
    }
    ctx->handler = handler;

    httpd_uri_t uri = {
        .uri               = path,
        .method            = HTTP_GET,
        .handler           = ws_shim_handler,
        .user_ctx          = ctx,
        .is_websocket      = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(h, &uri);
    if (err != ESP_OK) {
        free(ctx);
        bb_log_e(TAG, "register uri %s failed: %d", path, err);
        return err;
    }

    bb_log_i(TAG, "registered WS endpoint: %s", path);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_websocket_recv_frame
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_recv_frame(bb_http_request_t *req,
                                 bb_websocket_frame_t *frame,
                                 size_t max_len)
{
    if (!req || !frame) {
        return BB_ERR_INVALID_ARG;
    }
    httpd_req_t *http_req = (httpd_req_t *)req;
    httpd_ws_frame_t ws_pkt = {
        .final   = frame->final,
        .type    = (httpd_ws_type_t)frame->type,
        .payload = frame->payload,
        .len     = frame->len,
    };
    esp_err_t err = httpd_ws_recv_frame(http_req, &ws_pkt, max_len);
    if (err == ESP_OK) {
        frame->final   = ws_pkt.final;
        frame->type    = (bb_websocket_frame_type_t)ws_pkt.type;
        frame->payload = ws_pkt.payload;
        frame->len     = ws_pkt.len;
    }
    return err;
}

// ---------------------------------------------------------------------------
// bb_websocket_send_frame
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_send_frame(bb_http_request_t *req,
                                 const bb_websocket_frame_t *frame)
{
    if (!req || !frame) {
        return BB_ERR_INVALID_ARG;
    }
    httpd_req_t *http_req = (httpd_req_t *)req;
    httpd_ws_frame_t ws_pkt = {
        .final   = frame->final,
        .type    = (httpd_ws_type_t)frame->type,
        .payload = frame->payload,
        .len     = frame->len,
    };
    return httpd_ws_send_frame(http_req, &ws_pkt);
}

// ---------------------------------------------------------------------------
// Async broadcast — httpd_queue_work context
// ---------------------------------------------------------------------------

typedef struct {
    httpd_handle_t           server;
    int                      fd;
    uint8_t                 *payload;   // heap copy; freed after send
    size_t                   len;
    bb_websocket_frame_type_t type;
    bool                     final;
    bb_websocket_send_cb_t   cb;
    void                    *ctx;
} async_send_ctx_t;

static void async_send_worker(void *arg)
{
    async_send_ctx_t *a = (async_send_ctx_t *)arg;

    httpd_ws_frame_t ws_pkt = {
        .final   = a->final,
        .type    = (httpd_ws_type_t)a->type,
        .payload = a->payload,
        .len     = a->len,
    };

    esp_err_t err = httpd_ws_send_frame_async(a->server, a->fd, &ws_pkt);
    if (a->cb) {
        a->cb((bb_err_t)err, a->fd, a->ctx);
    }
    free(a->payload);
    free(a);
}

bb_err_t bb_websocket_broadcast_frame_async(bb_http_handle_t server,
                                            int fd,
                                            const bb_websocket_frame_t *frame,
                                            bb_websocket_send_cb_t cb,
                                            void *ctx)
{
    if (!server || !frame) {
        return BB_ERR_INVALID_ARG;
    }

    // Copy payload so the caller can free their buffer before the worker runs.
    uint8_t *payload_copy = NULL;
    if (frame->len > 0) {
        payload_copy = malloc(frame->len);
        if (!payload_copy) {
            return BB_ERR_NO_SPACE;
        }
        memcpy(payload_copy, frame->payload, frame->len);
    }

    async_send_ctx_t *a = malloc(sizeof(async_send_ctx_t));
    if (!a) {
        free(payload_copy);
        return BB_ERR_NO_SPACE;
    }
    a->server  = (httpd_handle_t)server;
    a->fd      = fd;
    a->payload = payload_copy;
    a->len     = frame->len;
    a->type    = frame->type;
    a->final   = frame->final;
    a->cb      = cb;
    a->ctx     = ctx;

    esp_err_t err = httpd_queue_work(a->server, async_send_worker, a);
    if (err != ESP_OK) {
        free(payload_copy);
        free(a);
        return (bb_err_t)err;
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_websocket_is_client
// ---------------------------------------------------------------------------

bool bb_websocket_is_client(bb_http_handle_t server, int fd)
{
    if (!server || fd < 0) {
        return false;
    }
    httpd_handle_t h = (httpd_handle_t)server;
    return httpd_ws_get_fd_info(h, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

// ---------------------------------------------------------------------------
// bb_websocket_broadcast_all
// ---------------------------------------------------------------------------

bb_err_t bb_websocket_broadcast_all(bb_http_handle_t server,
                                    const bb_websocket_frame_t *frame,
                                    bb_websocket_send_cb_t cb,
                                    void *ctx)
{
    if (!server || !frame) {
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
// bb_websocket_req_fd
// ---------------------------------------------------------------------------

int bb_websocket_req_fd(bb_http_request_t *req)
{
    if (!req) return -1;
    return httpd_req_to_sockfd((httpd_req_t *)req);
}

// ---------------------------------------------------------------------------
// bb_websocket_register_described_endpoint
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

#else // !CONFIG_HTTPD_WS_SUPPORT

// Stub implementations when WebSocket support is compiled out.
#include "bb_websocket.h"

bb_err_t bb_websocket_register_endpoint(bb_http_handle_t s, const char *p,
                                        bb_websocket_handler_fn h)
{ (void)s; (void)p; (void)h; return BB_ERR_UNSUPPORTED; }

bb_err_t bb_websocket_recv_frame(bb_http_request_t *req,
                                 bb_websocket_frame_t *f, size_t l)
{ (void)req; (void)f; (void)l; return BB_ERR_UNSUPPORTED; }

bb_err_t bb_websocket_send_frame(bb_http_request_t *req,
                                 const bb_websocket_frame_t *f)
{ (void)req; (void)f; return BB_ERR_UNSUPPORTED; }

bb_err_t bb_websocket_broadcast_frame_async(bb_http_handle_t s, int fd,
                                            const bb_websocket_frame_t *f,
                                            bb_websocket_send_cb_t cb, void *ctx)
{ (void)s; (void)fd; (void)f; (void)cb; (void)ctx; return BB_ERR_UNSUPPORTED; }

bb_err_t bb_websocket_broadcast_all(bb_http_handle_t s,
                                    const bb_websocket_frame_t *f,
                                    bb_websocket_send_cb_t cb, void *ctx)
{ (void)s; (void)f; (void)cb; (void)ctx; return BB_ERR_UNSUPPORTED; }

bool bb_websocket_is_client(bb_http_handle_t s, int fd)
{ (void)s; (void)fd; return false; }

int bb_websocket_req_fd(bb_http_request_t *req)
{ (void)req; return -1; }

bb_err_t bb_websocket_register_described_endpoint(bb_http_handle_t s,
                                                  const char *p,
                                                  bb_websocket_handler_fn h,
                                                  const bb_route_t *d)
{ (void)s; (void)p; (void)h; (void)d; return BB_ERR_UNSUPPORTED; }

#endif // CONFIG_HTTPD_WS_SUPPORT
#endif // ESP_PLATFORM
