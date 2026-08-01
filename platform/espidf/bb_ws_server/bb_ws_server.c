// ESP-IDF implementation of bb_ws_server.
// Wraps httpd_ws_* APIs (CONFIG_HTTPD_WS_SUPPORT, default y).
// bb_http_request_t is cast to httpd_req_t *; bb_http_handle_t to httpd_handle_t.
// Async broadcast uses httpd_queue_work + httpd_ws_send_frame_async.
//
// Portability guard: the entire file is compiled only when ESP_PLATFORM is
// defined (ensured by idf_component_register in CMakeLists.txt).

#ifdef ESP_PLATFORM
#include "sdkconfig.h"

#if CONFIG_HTTPD_WS_SUPPORT

#include "bb_ws_server.h"
#include "bb_http_server.h"
#include "bb_http_prov_gate.h"
#include "bb_log.h"
#include "bb_mem.h"

#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_idf_version.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

static const char *TAG = "bb_ws";

// B1-1348: bb_ws_server registers its httpd_uri_t directly (below) rather
// than going through bb_http.c's bb_shim_handler/api_dispatch_handler, so it
// does not automatically inherit their bb_http_prov_gate_check() insertion
// points -- see ws_pre_handshake_gate_cb and ws_shim_handler's own gate
// check below for where this component wires the gate in instead.
//
// The handshake-time gate (ws_pre_handshake_gate_cb) requires
// ws_pre_handshake_cb, which only exists on httpd_uri_t when ESP-IDF's own
// CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT is enabled (it depends on
// CONFIG_HTTPD_WS_SUPPORT, which this whole file is already gated on). If a
// consumer enables WS support without also enabling the pre-handshake-cb
// support, this component would silently ship the unauthenticated-handshake
// hole B1-1348 exists to close, with nothing else catching it -- fail the
// build loudly instead, same posture as the ESP_IDF_VERSION assert below.
// CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT itself does not exist before
// ESP-IDF v5.5.2 (added there; absent in 5.5.0/5.5.1) -- the pinned
// framework is 5.5.4 (see version.txt), so the #if below simply can't see
// the symbol and fails closed the same way on an older pin; noted here so
// a future pin rollback below 5.5.2 doesn't produce a confusing "#error
// fired for no visible reason" report.
#if !defined(CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT) || !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
#error "bb_ws_server requires CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT=y (depends on CONFIG_HTTPD_WS_SUPPORT; ESP-IDF >=5.5.2) to gate the WS handshake against provisioning state -- enable it in sdkconfig alongside CONFIG_HTTPD_WS_SUPPORT."
#endif

// The open-connection counter (below) depends on httpd invoking this uri
// handler once with req->method == HTTP_GET immediately after completing the
// WS handshake. ESP-IDF 6.0.1 removes that invocation (migration guide:
// "websocket handler no longer called during handshake" -> use
// ws_post_handshake_cb instead). We are on 5.5.x today; do not carry
// untested 6.0 code. Fail the build loudly instead of silently zeroing the
// counter if this component is ever compiled against >=6.0.
_Static_assert(ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0),
               "bb_ws_server open-connection counter relies on the pre-6.0 "
               "handshake handler invocation (req->method==HTTP_GET). "
               "ESP-IDF >=6.0.1 no longer calls the handler during the "
               "handshake -- reimplement the connect hook via "
               "ws_post_handshake_cb before bumping IDF.");

// ---------------------------------------------------------------------------
// Open-connection counter (B1-443)
// ---------------------------------------------------------------------------
// Incremented once per WS session at handshake completion, decremented when
// httpd tears the session down. httpd calls free_ctx exactly once per session
// when it frees req->sess_ctx (socket close, timeout, or server shutdown),
// giving a reliable disconnect hook without polling the fd table per read.
static atomic_size_t s_ws_open_count = 0;

// ---------------------------------------------------------------------------
// Disconnect notification (B1-hardening: WS-egress subscription-state clear
// on disconnect, not just suspend)
// ---------------------------------------------------------------------------
// Reuses the same req->sess_ctx/free_ctx hook as the open-connection counter
// above: sess_ctx is set to (fd + 1) (a non-NULL marker that also carries the
// fd) at handshake completion, and decoded back out in ws_session_free_ctx so
// the disconnect callback knows which fd went away without any extra
// per-session state.
static bb_ws_server_disconnect_cb_t s_disconnect_cb  = NULL;
static void                        *s_disconnect_ctx = NULL;

void bb_ws_server_set_disconnect_cb(bb_ws_server_disconnect_cb_t cb, void *ctx)
{
    s_disconnect_cb  = cb;
    s_disconnect_ctx = ctx;
}

// ---------------------------------------------------------------------------
// Connect notification -- fires from the same connect-signal branch
// (req->sess_ctx == NULL) that arms the disconnect hook above, before any
// data frame has been received from the new client.
// ---------------------------------------------------------------------------
static bb_ws_server_connect_cb_t s_connect_cb  = NULL;
static void                     *s_connect_ctx = NULL;

void bb_ws_server_set_connect_cb(bb_ws_server_connect_cb_t cb, void *ctx)
{
    s_connect_cb  = cb;
    s_connect_ctx = ctx;
}

static void ws_session_free_ctx(void *ctx)
{
    atomic_fetch_sub(&s_ws_open_count, 1);
    int fd = (int)((intptr_t)ctx - 1);
    if (s_disconnect_cb) {
        s_disconnect_cb(fd, s_disconnect_ctx);
    }
}

size_t bb_ws_server_open_count(void)
{
    return atomic_load(&s_ws_open_count);
}

// ---------------------------------------------------------------------------
// Internal shim — bridges httpd_uri_t handler to bb_ws_server_handler_fn
// ---------------------------------------------------------------------------

// Per-endpoint user_ctx stored on the httpd_uri_t. `path` is the same
// pointer bb_ws_server_register_endpoint() was called with (not copied --
// same "must have static/persistent storage duration" contract as
// bb_http_prov_gate_allow()'s own path parameter). This is the gate check's
// SOLE source of the endpoint's path -- see the comment on req->uri below
// for why req->uri itself cannot be used for that.
typedef struct {
    bb_ws_server_handler_fn handler;
    const char              *path;
} ws_endpoint_ctx_t;

// req->uri is NOT a reliable source for the endpoint path in either gate
// check below, despite being correctly populated for the very first
// (handshake) call -- verified against the vendored ESP-IDF 5.5.4 source
// (components/esp_http_server/src/httpd_parse.c):
//   - httpd_req_new() calls init_req() UNCONDITIONALLY on every single call
//     for a session (init_req() memsets r->uri to '\0' at the top, plus
//     zeroes r->user_ctx -- see below for why user_ctx survives anyway).
//   - For an established WS session (sd->ws_handshake_done &&
//     sd->ws_handler != NULL), httpd_req_new() then calls sd->ws_handler(r)
//     DIRECTLY (the is_websocket fast path) and returns -- it never calls
//     httpd_parse_req()/httpd_uri(), which is the ONLY code path that ever
//     populates req->uri. So req->uri is reliably "" on every real data
//     frame, and only ever correct on the handshake-completion call
//     (dispatched through httpd_uri(), which parses the request first).
//     ESP-IDF's own httpd_parse.c treats this as expected behavior: two of
//     its own functions guard `if (r->uri[0] == '\0')` for exactly this
//     mid-session case.
//   - req->user_ctx, by contrast, IS reliably repopulated on every call:
//     httpd_req_new() sets `r->user_ctx = sd->ws_user_ctx;` unconditionally,
//     immediately after init_req() and BEFORE the fast-path branch above,
//     for every single invocation (handshake call AND every data frame).
//     sd->ws_user_ctx is set once, at handshake time, from the registered
//     httpd_uri_t.user_ctx (see httpd_uri.c) -- i.e. from `ctx` below.
// A per-frame provisioning check keyed on req->uri would therefore
// evaluate against "" for every real data frame, which bb_route_uri_match
// can never match against any allowlist entry -- silently tearing down an
// ALLOWLISTED session's first real frame while provisioning is active,
// defeating the allowlist case the gate exists to support. Both gate
// checks below use ctx->path instead, which is available and correct via
// req->user_ctx on every call, including the fast path.

// Provisioning gate — handshake insertion point. Registered as
// httpd_uri_t.ws_pre_handshake_cb (see bb_ws_server_register_endpoint
// below), so ESP-IDF's httpd_uri() calls this BEFORE
// httpd_ws_respond_server_handshake() ever runs (see httpd_uri.c) — a
// denied request never receives the WS upgrade response at all, and the
// connection is torn down exactly like any other rejected handshake. This
// is the only insertion point that can prevent the handshake itself; by the
// time ws_shim_handler (below) first runs, the upgrade has already
// completed at the protocol level.
//
// Response shape: at this point httpd_uri() has NOT yet called
// httpd_ws_respond_server_handshake() (see httpd_uri.c's call order --
// ws_pre_handshake_cb runs first, and only proceeds to the WS handshake
// response if it returns ESP_OK), so req is still an ordinary,
// not-yet-upgraded HTTP request. httpd_resp_send_err()/httpd_resp_send()
// have no dependency on WS handshake state (httpd_valid_req() is a
// pointer-sanity debug check, gated off by default, unrelated) -- sending a
// normal HTTP response from here is exactly as valid as from any other
// handler. Without an explicit response, returning ESP_FAIL alone writes
// ZERO bytes before httpd tears the socket down, which is trivially
// distinguishable on the wire from a genuinely-unregistered WS path (which
// gets httpd's own formatted 404 via httpd_req_handle_err) -- an
// unauthenticated SoftAP client could fingerprint "a WS endpoint exists
// here" from a bare rejected handshake alone, exactly the leak
// bb_http.c's sibling insertion points were built to prevent (see e.g.
// api_dispatch_handler's own comment). usr_msg is deliberately NULL, not a
// custom string: NULL makes httpd_resp_send_err() emit its own default
// "Nothing matches the given URI" body (see httpd_txrx.c), which is
// BYTE-IDENTICAL to what httpd_uri()'s own uri==NULL branch sends for a
// genuinely-unregistered path (also NULL usr_msg, via
// httpd_req_handle_err's no-custom-handler default) -- the correct "genuine
// miss" comparator for a WS endpoint, which is registered directly with
// httpd rather than through bb_http.c's own route registry/dispatch table.
static esp_err_t ws_pre_handshake_gate_cb(httpd_req_t *req)
{
    // req->user_ctx is set (from httpd_uri_t.user_ctx) before this callback
    // runs -- see httpd_uri.c's call order (req->user_ctx assignment
    // precedes the WS-handshake block that invokes ws_pre_handshake_cb) --
    // so ctx is always non-NULL in practice for a route this component
    // registered itself. Fail closed (deny) rather than fall back to
    // req->uri if that invariant is ever violated: this is a security gate,
    // and a silent fallback here is exactly the kind of "looks fine, quietly
    // wrong" path this fix exists to eliminate (see the block comment above
    // for why req->uri is not a trustworthy source in general).
    ws_endpoint_ctx_t *ctx = (ws_endpoint_ctx_t *)req->user_ctx;
    if (!ctx) {
        bb_log_e(TAG, "ws pre-handshake gate: NULL user_ctx (unreachable in practice)");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }
    if (!bb_http_prov_gate_check(BB_HTTP_GET, ctx->path)) {
        bb_log_w(TAG, "ws handshake denied during provisioning: %s", ctx->path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ws_shim_handler(httpd_req_t *req)
{
    ws_endpoint_ctx_t *ctx = (ws_endpoint_ctx_t *)req->user_ctx;
    if (!ctx || !ctx->handler) {
        return ESP_FAIL;
    }

    // Provisioning gate — established-connection insertion point. A WS
    // session is long-lived: it can complete its handshake (and pass
    // ws_pre_handshake_gate_cb above) while provisioning is inactive, then
    // provisioning can become active later while the session is still open.
    // ws_shim_handler runs on every subsequent call for that session (the
    // handshake-completion call below AND every real data frame after it),
    // so re-checking here closes that window instead of only gating the
    // handshake once. Returning ESP_FAIL closes the underlying socket,
    // tearing down a session that was allowed in before provisioning
    // started gating it.
    //
    // Response shape, deliberately NOT matching ws_pre_handshake_gate_cb's
    // explicit-404 fix above: every call into this handler happens AFTER
    // httpd_ws_respond_server_handshake() has already sent the 101 upgrade
    // response (see httpd_uri.c's call order -- ws_pre_handshake_gate_cb
    // runs and must return ESP_OK BEFORE httpd ever calls this handler at
    // all, for both the handshake-completion call and every later frame).
    // The client already considers the connection a live WebSocket by the
    // time this code runs, so there is no "genuine miss" 404 to imitate --
    // writing an httpd_resp_send_err()-shaped plain-HTTP response onto an
    // already-upgraded socket would not produce a clean deny, it would
    // inject malformed bytes into the WS frame stream the client is now
    // parsing (worse than no response, and protocol-incoherent). Nor does
    // this leak the "endpoint exists" information ws_pre_handshake_gate_cb
    // protects against: reaching this handler at all requires having
    // already completed a real WS handshake for this exact path, so the
    // client calling in here already has definitive, first-party knowledge
    // the endpoint exists -- closing the socket bare (no body) is the only
    // protocol-coherent deny available here, and it discloses nothing this
    // client doesn't already know. See test_bb_ws_server_prov_gate_* in
    // test/test_host/test_bb_ws_server.c, which pins exactly this: a denied
    // frame closes with no captured response, while a denied handshake
    // captures the 404.
    //
    // WS endpoints are always registered on HTTP_GET (see
    // bb_ws_server_register_endpoint below) regardless of what req->method
    // carries during this call — it is HTTP_GET only for the
    // handshake-completion call; real data frames arrive via the
    // is_websocket fast-path with method 0 / HTTP_DELETE, never HTTP_GET
    // (see the comment further down) — so the check always evaluates
    // BB_HTTP_GET rather than trying to map req->method.
    //
    // ctx->path, NOT req->uri: req->uri is only ever populated for the
    // handshake-completion call (dispatched through httpd_uri(), which
    // parses the request) -- every real data frame takes the is_websocket
    // fast path in httpd_req_new(), which never calls the parser, leaving
    // req->uri reliably "" (see the block comment above ws_pre_handshake_
    // gate_cb for the full vendored-source trace). Checking against ""
    // would silently deny every frame of an ALLOWLISTED session once it's
    // past the handshake -- ctx->path is populated once at registration and
    // is reliably reachable via req->user_ctx on every call, including the
    // fast path.
    if (!bb_http_prov_gate_check(BB_HTTP_GET, ctx->path)) {
        bb_log_w(TAG, "ws frame denied during provisioning: %s", ctx->path);
        return ESP_FAIL;
    }

    // httpd handles the HTTP→WS upgrade internally on the first GET that
    // triggers is_websocket.  Subsequent calls carry actual frame data.
    // Why this works (pre-6.0 only, see _Static_assert above): after the WS
    // handshake completes, httpd falls through and invokes this uri->handler
    // exactly once more with req->method == HTTP_GET to let the app do
    // connect-time setup; real data frames are then delivered via the
    // is_websocket fast-path with method 0 / HTTP_DELETE, never HTTP_GET.
    if (req->method == HTTP_GET) {
        // Upgrade request — httpd has completed the handshake at this point.
        // sess_ctx is NULL on the first (and only) HTTP_GET call for this
        // session; use it as the connect signal and arm free_ctx as the
        // matching disconnect signal.
        if (req->sess_ctx == NULL) {
            int fd = httpd_req_to_sockfd(req);
            if (fd < 0) {
                // (fd + 1) would encode to NULL here, colliding with the
                // "unset/fresh-handshake" sentinel above -- a later teardown
                // would then decode fd=-1 and client_sub_clear(-1) would be a
                // silent no-op, leaking the subscription slot (the fd-reuse
                // bleed this hook exists to prevent). Fail loud instead.
                bb_log_e(TAG, "httpd_req_to_sockfd failed at handshake");
                return ESP_FAIL;
            }
            atomic_fetch_add(&s_ws_open_count, 1);
            // (fd + 1) as a non-NULL marker: also carries fd through to
            // ws_session_free_ctx for the disconnect callback above.
            req->sess_ctx = (void *)(intptr_t)(fd + 1);
            req->free_ctx = ws_session_free_ctx;
            if (s_connect_cb) {
                s_connect_cb((bb_http_handle_t)req->handle, fd, s_connect_ctx);
            }
        }
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
        buf = bb_calloc_prefer_spiram(1, ws_pkt.len + 1);
        if (!buf) {
            bb_log_e(TAG, "ws recv alloc failed (%u bytes)", (unsigned)ws_pkt.len);
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (err != ESP_OK) {
            bb_log_w(TAG, "ws recv frame failed: %d", err);
            bb_mem_free(buf);
            return err;
        }
    }

    // Build portable frame descriptor and call the user handler.
    bb_ws_server_frame_t frame = {
        .final   = ws_pkt.final,
        .type    = (bb_ws_server_frame_type_t)ws_pkt.type,
        .payload = ws_pkt.payload,
        .len     = ws_pkt.len,
    };

    bb_err_t rc = ctx->handler((bb_http_request_t *)req, &frame);

    bb_mem_free(buf);
    return (rc == BB_OK) ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------------------
// bb_ws_server_register_endpoint
// ---------------------------------------------------------------------------

bb_err_t bb_ws_server_register_endpoint(bb_http_handle_t server,
                                        const char *path,
                                        bb_ws_server_handler_fn handler)
{
    if (!server || !path || !handler) {
        return BB_ERR_INVALID_ARG;
    }

    httpd_handle_t h = (httpd_handle_t)server;

    // ctx is never freed (httpd lifetime == server lifetime); acceptable for
    // the endpoint count (small; same pattern as bb_event_routes SSE ctx).
    ws_endpoint_ctx_t *ctx = bb_malloc_prefer_spiram(sizeof(ws_endpoint_ctx_t));
    if (!ctx) {
        bb_log_e(TAG, "ctx alloc failed for %s", path);
        return BB_ERR_NO_SPACE;
    }
    ctx->handler = handler;
    ctx->path    = path;

    httpd_uri_t uri = {
        .uri               = path,
        .method            = HTTP_GET,
        .handler           = ws_shim_handler,
        .user_ctx          = ctx,
        .is_websocket      = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL,
        .ws_pre_handshake_cb = ws_pre_handshake_gate_cb,
    };

    esp_err_t err = httpd_register_uri_handler(h, &uri);
    if (err != ESP_OK) {
        bb_mem_free(ctx);
        bb_log_e(TAG, "register uri %s failed: %d", path, err);
        return err;
    }

    bb_log_i(TAG, "registered WS endpoint: %s", path);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_ws_server_recv_frame
// ---------------------------------------------------------------------------

bb_err_t bb_ws_server_recv_frame(bb_http_request_t *req,
                                 bb_ws_server_frame_t *frame,
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
        frame->type    = (bb_ws_server_frame_type_t)ws_pkt.type;
        frame->payload = ws_pkt.payload;
        frame->len     = ws_pkt.len;
    }
    return err;
}

// ---------------------------------------------------------------------------
// bb_ws_server_send_frame
// ---------------------------------------------------------------------------

bb_err_t bb_ws_server_send_frame(bb_http_request_t *req,
                                 const bb_ws_server_frame_t *frame)
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
    bb_ws_server_frame_type_t type;
    bool                     final;
    bb_ws_server_send_cb_t   cb;
    void                    *ctx;
} async_send_ctx_t;

static void async_send_worker(void *arg)
{
    async_send_ctx_t *a = (async_send_ctx_t *)arg;

    // TOCTOU re-check (B1-hardening): fds are snapshotted by the caller
    // under its own lock, then this work item is queued and runs later on
    // httpd's async-send worker task.
    // If the fd closed in the meantime, httpd_ws_send_frame_async would send
    // into a dead socket; re-validating here with httpd_ws_get_fd_info
    // (via bb_ws_server_is_client) closes that window. It does NOT close the
    // narrower window where the same fd number was already reused by a NEW
    // WS client before this worker runs -- get_fd_info reports WEBSOCKET for
    // that new client too, so it looks "live" either way. That residual case
    // delivers at most one stray broadcast telemetry frame (broadcast-public
    // data, bounded by this worker-queue's latency) to the wrong client; it
    // is accepted and tracked as a follow-up, not fixed with a generation
    // counter here.
    if (!bb_ws_server_is_client((bb_http_handle_t)a->server, a->fd)) {
        bb_log_d(TAG, "async send skipped: fd %d no longer a live WS client", a->fd);
        if (a->cb) {
            a->cb((bb_err_t)ESP_ERR_INVALID_STATE, a->fd, a->ctx);
        }
        bb_mem_free(a->payload);
        bb_mem_free(a);
        return;
    }

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
    bb_mem_free(a->payload);
    bb_mem_free(a);
}

bb_err_t bb_ws_server_broadcast_frame_async(bb_http_handle_t server,
                                            int fd,
                                            const bb_ws_server_frame_t *frame,
                                            bb_ws_server_send_cb_t cb,
                                            void *ctx)
{
    if (!server || !frame) {
        return BB_ERR_INVALID_ARG;
    }

    // Copy payload so the caller can free their buffer before the worker runs.
    uint8_t *payload_copy = NULL;
    if (frame->len > 0) {
        payload_copy = bb_malloc_prefer_spiram(frame->len);
        if (!payload_copy) {
            return BB_ERR_NO_SPACE;
        }
        memcpy(payload_copy, frame->payload, frame->len);
    }

    async_send_ctx_t *a = bb_malloc_prefer_spiram(sizeof(async_send_ctx_t));
    if (!a) {
        bb_mem_free(payload_copy);
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
        bb_mem_free(payload_copy);
        bb_mem_free(a);
        return (bb_err_t)err;
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_ws_server_is_client
// ---------------------------------------------------------------------------

bool bb_ws_server_is_client(bb_http_handle_t server, int fd)
{
    if (!server || fd < 0) {
        return false;
    }
    httpd_handle_t h = (httpd_handle_t)server;
    return httpd_ws_get_fd_info(h, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

// ---------------------------------------------------------------------------
// bb_ws_server_broadcast_all
// ---------------------------------------------------------------------------

bb_err_t bb_ws_server_broadcast_all(bb_http_handle_t server,
                                    const bb_ws_server_frame_t *frame,
                                    bb_ws_server_send_cb_t cb,
                                    void *ctx)
{
    if (!server || !frame) {
        return BB_ERR_INVALID_ARG;
    }
    bb_err_t last_err = BB_OK;
    for (int fd = 0; fd < BB_WS_SERVER_MAX_FD; fd++) {
        if (bb_ws_server_is_client(server, fd)) {
            bb_err_t err = bb_ws_server_broadcast_frame_async(server, fd, frame, cb, ctx);
            if (err != BB_OK) {
                last_err = err;
            }
        }
    }
    return last_err;
}

// ---------------------------------------------------------------------------
// bb_ws_server_req_fd
// ---------------------------------------------------------------------------

int bb_ws_server_req_fd(bb_http_request_t *req)
{
    if (!req) return -1;
    return httpd_req_to_sockfd((httpd_req_t *)req);
}

// ---------------------------------------------------------------------------
// bb_ws_server_register_described_endpoint
// ---------------------------------------------------------------------------

bb_err_t bb_ws_server_register_described_endpoint(bb_http_handle_t server,
                                                  const char *path,
                                                  bb_ws_server_handler_fn handler,
                                                  const bb_route_t *descriptor)
{
    if (!path || !handler || !descriptor) {
        return BB_ERR_INVALID_ARG;
    }
    bb_err_t err = bb_ws_server_register_endpoint(server, path, handler);
    if (err != BB_OK) {
        return err;
    }
    return bb_http_register_route_descriptor_only(descriptor);
}

// ---------------------------------------------------------------------------
// bb_ws_server_close_client
// ---------------------------------------------------------------------------

bb_err_t bb_ws_server_close_client(bb_http_handle_t server, int fd)
{
    if (!server || fd < 0) return BB_ERR_INVALID_ARG;
    esp_err_t e = httpd_sess_trigger_close((httpd_handle_t)server, fd);
    return (bb_err_t)e;
}

#else // !CONFIG_HTTPD_WS_SUPPORT

// Stub implementations when WebSocket support is compiled out.
#include "bb_ws_server.h"

bb_err_t bb_ws_server_register_endpoint(bb_http_handle_t s, const char *p,
                                        bb_ws_server_handler_fn h)
{ (void)s; (void)p; (void)h; return BB_ERR_UNSUPPORTED; }

bb_err_t bb_ws_server_recv_frame(bb_http_request_t *req,
                                 bb_ws_server_frame_t *f, size_t l)
{ (void)req; (void)f; (void)l; return BB_ERR_UNSUPPORTED; }

bb_err_t bb_ws_server_send_frame(bb_http_request_t *req,
                                 const bb_ws_server_frame_t *f)
{ (void)req; (void)f; return BB_ERR_UNSUPPORTED; }

bb_err_t bb_ws_server_broadcast_frame_async(bb_http_handle_t s, int fd,
                                            const bb_ws_server_frame_t *f,
                                            bb_ws_server_send_cb_t cb, void *ctx)
{ (void)s; (void)fd; (void)f; (void)cb; (void)ctx; return BB_ERR_UNSUPPORTED; }

bb_err_t bb_ws_server_broadcast_all(bb_http_handle_t s,
                                    const bb_ws_server_frame_t *f,
                                    bb_ws_server_send_cb_t cb, void *ctx)
{ (void)s; (void)f; (void)cb; (void)ctx; return BB_ERR_UNSUPPORTED; }

bool bb_ws_server_is_client(bb_http_handle_t s, int fd)
{ (void)s; (void)fd; return false; }

int bb_ws_server_req_fd(bb_http_request_t *req)
{ (void)req; return -1; }

bb_err_t bb_ws_server_register_described_endpoint(bb_http_handle_t s,
                                                  const char *p,
                                                  bb_ws_server_handler_fn h,
                                                  const bb_route_t *d)
{ (void)s; (void)p; (void)h; (void)d; return BB_ERR_UNSUPPORTED; }

bb_err_t bb_ws_server_close_client(bb_http_handle_t s, int fd)
{ (void)s; (void)fd; return BB_ERR_UNSUPPORTED; }

size_t bb_ws_server_open_count(void)
{ return 0; }

void bb_ws_server_set_disconnect_cb(bb_ws_server_disconnect_cb_t cb, void *ctx)
{ (void)cb; (void)ctx; }

#endif // CONFIG_HTTPD_WS_SUPPORT
#endif // ESP_PLATFORM
