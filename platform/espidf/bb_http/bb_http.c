#include "bb_http.h"
#include "bb_http_api_dispatch.h"
#include "bb_http_status.h"
#include "bb_json.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/socket.h>
#include "lwip/sockets.h"

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;
static int s_max_uri_handlers = 0;        // set by ensure_started
static int s_registered_handlers = 0;     // incremented on every successful register

/* No-op handler for ESP_HTTP_SERVER_EVENT (B1-306).
 * esp_http_server posts ESP_HTTP_SERVER_EVENT to the default event loop on every
 * HTTP event (connect, header, sent-data, etc.). Without a registered handler,
 * esp_event logs "no handlers have been registered for event ESP_HTTP_SERVER_EVENT:N"
 * at DEBUG. With /api/logs SSE open this self-amplifies: each streamed line
 * triggers another SENT_DATA event → another DEBUG log → streamed again, causing
 * thousands of log lines/sec. Registering this no-op suppresses that spam. */
static void bb_http_noop_event_handler(void *arg, esp_event_base_t base,
                                       int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
}

/* Vestigial since /api dispatch; kept for ABI — PRE_HTTP companions still call
 * it but route count no longer drives max_uri_handlers. */
void bb_http_reserve_routes(int n)
{
    (void)n;
}

static const char *s_cors_methods = "GET, POST, OPTIONS";
static const char *s_cors_headers = "Content-Type";

// Helper to convert bb_http_method_t to string
static const char *bb_http_method_str(bb_http_method_t method)
{
    switch (method) {
        case BB_HTTP_GET:     return "GET";
        case BB_HTTP_POST:    return "POST";
        case BB_HTTP_PATCH:   return "PATCH";
        case BB_HTTP_PUT:     return "PUT";
        case BB_HTTP_DELETE:  return "DELETE";
        case BB_HTTP_OPTIONS: return "OPTIONS";
        default:              return "UNKNOWN";
    }
}

void bb_http_set_cors_methods(const char *methods)
{
    s_cors_methods = methods ? methods : "GET, POST, OPTIONS";
}

void bb_http_set_cors_headers(const char *headers)
{
    s_cors_headers = headers ? headers : "Content-Type";
}

static esp_err_t preflight_handler(httpd_req_t *req);
static esp_err_t api_dispatch_handler(httpd_req_t *req);
static esp_err_t method_not_allowed_err_handler(httpd_req_t *req, httpd_err_code_t err);

// One-shot work item queued on the httpd worker thread immediately after server
// start.  Calling lwip_socket_thread_init() here pre-allocates the per-thread
// LWIP TLS semaphore while heap is plentiful, eliminating the lazy-alloc-under-
// load path that aborts the device under B1-223.
static void prealloc_tls_sem_cb(void *arg)
{
    (void)arg;
    lwip_socket_thread_init();
    bb_log_i(TAG, "httpd lwip TLS sem pre-allocated");
}

// Ensure the shared HTTP server is started. Exposed for bb_prov to call.
// Low-level helper; most consumers should use bb_http_server_start instead.
bb_err_t bb_http_server_ensure_started(void)
{
    if (s_server) return BB_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets =
#ifdef CONFIG_BB_HTTP_MAX_OPEN_SOCKETS
        CONFIG_BB_HTTP_MAX_OPEN_SOCKETS;
#else
        9;
#endif
    // Clamp to LWIP socket pool: httpd reserves CONFIG_BB_HTTP_LWIP_RESERVE sockets
    // for non-httpd usage (stratum, mDNS, transient outbound). If httpd exceeds
    // CONFIG_LWIP_MAX_SOCKETS - CONFIG_BB_HTTP_LWIP_RESERVE, httpd_start will abort
    // (the assert message is "Config option max_open_sockets is too large").
    // Surface a warning so consumers can tune intentionally instead of getting
    // a runtime panic during prov-mode startup.
    {
        const int lwip_cap = CONFIG_LWIP_MAX_SOCKETS - CONFIG_BB_HTTP_LWIP_RESERVE;
        if (config.max_open_sockets > lwip_cap) {
            bb_log_w(TAG, "capping max_open_sockets %d -> %d (CONFIG_LWIP_MAX_SOCKETS=%d, reserve=%d)",
                     config.max_open_sockets, lwip_cap, CONFIG_LWIP_MAX_SOCKETS, CONFIG_BB_HTTP_LWIP_RESERVE);
            config.max_open_sockets = lwip_cap;
        }
    }
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.recv_wait_timeout =
#ifdef CONFIG_BB_HTTP_RECV_WAIT_TIMEOUT_S
        CONFIG_BB_HTTP_RECV_WAIT_TIMEOUT_S;
#else
        10;
#endif
    config.send_wait_timeout =
#ifdef CONFIG_BB_HTTP_SEND_WAIT_TIMEOUT_S
        CONFIG_BB_HTTP_SEND_WAIT_TIMEOUT_S;
#else
        10;
#endif
    config.uri_match_fn = httpd_uri_match_wildcard;
#ifdef CONFIG_BB_HTTP_TASK_CORE_ID
    if (CONFIG_BB_HTTP_TASK_CORE_ID >= 0) {
        config.core_id = CONFIG_BB_HTTP_TASK_CORE_ID;
    }
#endif

    // max_uri_handlers is now a single constant knob (BB_HTTP_MAX_URI_HANDLERS,
    // default 12). /api/* routes do NOT consume httpd handler slots — they are
    // dispatched via the bb_api_dispatch table instead. The 12-slot default
    // covers: GET/POST/PUT/PATCH/DELETE /api/* (5) + OPTIONS /* (1) + GET /* (1)
    // + headroom for non-/api routes (/save, captive /*) registered by consumers.
    // bb_http_reserve_routes() is now vestigial; route count no longer drives
    // this value.
#ifdef CONFIG_BB_HTTP_MAX_URI_HANDLERS
    int cap = CONFIG_BB_HTTP_MAX_URI_HANDLERS;
#else
    int cap = 12;
#endif

    config.max_uri_handlers = cap;
    s_max_uri_handlers = cap;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;

    // Suppress esp_event "no handlers have been registered for event
    // ESP_HTTP_SERVER_EVENT:N" DEBUG spam (B1-306). Without this, every HTTP
    // event posts to the default loop with no consumer, flooding the log at
    // DEBUG — especially when /api/logs SSE is open (self-amplifying loop).
    esp_err_t evt_err = esp_event_handler_register(
        ESP_HTTP_SERVER_EVENT, ESP_EVENT_ANY_ID, bb_http_noop_event_handler, NULL);
    if (evt_err != ESP_OK) {
        bb_log_d(TAG, "esp_event register noop failed: %s (non-fatal)",
                 esp_err_to_name(evt_err));
    }

    // Register a custom 405 error handler that distinguishes "real 405" (URI is
    // registered for a different method) from "URI not registered at all" (should
    // be 404). Without this, the OPTIONS /* CORS preflight wildcard causes httpd
    // to return 405 for every unregistered URI on non-OPTIONS requests.
    httpd_register_err_handler(s_server, HTTPD_405_METHOD_NOT_ALLOWED,
                               method_not_allowed_err_handler);

    /* B1-223 (root cause C): lwip lazily allocates a per-thread TLS semaphore
     * the first time sys_thread_sem_get() is called on a thread.  Under heap
     * pressure during the ~200 lwip_send calls of a large /api/pool response
     * the alloc can return NULL, which triggers the LWIP_ASSERT in
     * tcpip_send_msg_wait_sem (tcpip.c:453) and aborts the device.
     *
     * Fix: queue a one-shot work item that runs on the httpd worker thread
     * immediately after server start, when heap is still plentiful, so the
     * sem is pre-allocated before any real request arrives.
     * LWIP_NETCONN_SEM_PER_THREAD must be 1 (shipped via PR #301). */
    httpd_queue_work(s_server, prealloc_tls_sem_cb, NULL);

    httpd_uri_t preflight = { .uri = "/*", .method = HTTP_OPTIONS, .handler = preflight_handler };
    err = httpd_register_uri_handler(s_server, &preflight);
    if (err == ESP_OK) {
        s_registered_handlers++;
    } else {
        bb_log_e(TAG, "register route failed (OPTIONS /*): %s", esp_err_to_name(err));
    }

    // Register per-method /api/* wildcards that dispatch via bb_api_dispatch.
    // INVARIANT: these must be registered before the asset GET /* wildcard
    // (bb_http_register_assets), because esp_http_server uses first-registered-
    // first-matched semantics for wildcard URIs. ensure_started() runs before
    // any consumer calls bb_http_register_assets(), so the ordering is guaranteed.
    static const int s_api_methods[] = {
        HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_PATCH, HTTP_DELETE
    };
    for (size_t mi = 0; mi < sizeof(s_api_methods)/sizeof(s_api_methods[0]); mi++) {
        httpd_uri_t api_uri = {
            .uri     = "/api/*",
            .method  = s_api_methods[mi],
            .handler = api_dispatch_handler,
            .user_ctx = NULL,
        };
        esp_err_t merr = httpd_register_uri_handler(s_server, &api_uri);
        if (merr == ESP_OK) {
            s_registered_handlers++;
        } else {
            bb_log_e(TAG, "register route failed (/api/* method %d): %s",
                     s_api_methods[mi], esp_err_to_name(merr));
        }
    }

    bb_log_i(TAG, "HTTP server started, max_uri_handlers=%d", cap);
    return BB_OK;
}

// Internal helper: cast opaque handle back to httpd_handle_t for internal use
static inline httpd_handle_t _bb_handle_to_internal(bb_http_handle_t h) {
    return (httpd_handle_t)h;
}

static void set_common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
}

static esp_err_t preflight_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", s_cors_methods);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", s_cors_headers);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Custom 405 error handler: distinguishes between a genuine "method not
// allowed" (the URI IS registered, just for a different HTTP method) and a
// plain "not found" (the URI is not registered at all).
//
// Root cause: OPTIONS /* (CORS preflight) is a wildcard that httpd matches
// against every URI. When a non-OPTIONS request arrives for an unregistered
// path, httpd finds no exact handler, but the OPTIONS /* wildcard "wins" the
// method check, so it returns 405 instead of 404. This handler corrects that:
// if the requested URI is not present in our route registry for ANY method
// (excluding the internal OPTIONS/* and GET/* catch-all wildcards), we send
// 404. Only genuine method-mismatch requests (e.g. GET on a POST-only route)
// keep the 405.
static esp_err_t method_not_allowed_err_handler(httpd_req_t *req,
                                                httpd_err_code_t err)
{
    (void)err;
    if (!bb_http_uri_is_registered(req->uri)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    } else {
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }
    return ESP_OK;
}

// Wildcard handler for GET/POST/PUT/PATCH/DELETE /api/*.
// Maps the httpd method integer back to bb_http_method_t, strips the query
// string from req->uri, then looks up in bb_api_dispatch.
//   HIT             → wrap req as bb_http_request_t* and call the handler.
//   METHOD_MISMATCH → 405 with JSON body.
//   MISS            → 404 with JSON body.
// SSE and async handlers work unchanged: their fn(req) calls
// bb_http_req_async_handler_begin + task-spawn internally.
static esp_err_t api_dispatch_handler(httpd_req_t *req)
{
    // Map httpd method integer → bb_http_method_t.
    bb_http_method_t method;
    switch (req->method) {
        case HTTP_GET:    method = BB_HTTP_GET;    break;
        case HTTP_POST:   method = BB_HTTP_POST;   break;
        case HTTP_PUT:    method = BB_HTTP_PUT;    break;
        case HTTP_PATCH:  method = BB_HTTP_PATCH;  break;
        case HTTP_DELETE: method = BB_HTTP_DELETE; break;
        default:
            httpd_resp_set_status(req, "405 Method Not Allowed");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"method not allowed\"}");
            return ESP_OK;
    }

    // Strip query string: dispatch lookup accepts URI with query but doing it
    // here avoids duplicate work across many handlers.
    const char *uri = req->uri;

    bb_http_handler_fn handler = NULL;
    bb_api_dispatch_result_t res = bb_api_dispatch_lookup(method, uri, &handler);

    if (res == BB_API_DISPATCH_HIT) {
        // Wrap httpd_req_t* as bb_http_request_t* — identical cast used by
        // bb_shim_handler. The fn(req) contract is the same for sync and async
        // (SSE/async) handlers.
        bb_err_t herr = handler((bb_http_request_t *)req);
        return herr == BB_OK ? ESP_OK : ESP_FAIL;
    }

    if (res == BB_API_DISPATCH_METHOD_MISMATCH) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"method not allowed\"}");
        return ESP_OK;
    }

    // MISS
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"not found\"}");
    return ESP_OK;
}

esp_err_t bb_http_server_start(void)
{
    esp_err_t err = bb_http_server_ensure_started();
    if (err != ESP_OK) {
        bb_log_e(TAG, "failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    bb_log_i(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

esp_err_t bb_http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}

bb_http_handle_t bb_http_server_get_handle(void)
{
    return (bb_http_handle_t)s_server;
}

// ============================================================================
// PORTABLE API IMPLEMENTATIONS
// ============================================================================

// Shim handler adapter: translates bb_http_handler_fn ↔ httpd_req_t*
static esp_err_t bb_shim_handler(httpd_req_t *req)
{
    static bool s_core_logged = false;
    if (!s_core_logged) {
        s_core_logged = true;
        bb_log_i("bb_http", "httpd worker on core %d", xPortGetCoreID());
    }
    bb_http_handler_fn fn = (bb_http_handler_fn)req->user_ctx;
    if (!fn) return ESP_FAIL;
    return fn((bb_http_request_t*)req) == BB_OK ? ESP_OK : ESP_FAIL;
}

// Asset table used by the single wildcard handler.
static const bb_http_asset_t *s_assets      = NULL;
static size_t                 s_asset_count = 0;

// Serve a single asset: set headers and send bytes.
static esp_err_t bb_http_serve_asset(httpd_req_t *req, const bb_http_asset_t *asset)
{
    httpd_resp_set_type(req, asset->mime);
    if (asset->encoding) {
        httpd_resp_set_hdr(req, "Content-Encoding", asset->encoding);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=300");
    return httpd_resp_send(req, (const char*)asset->data, asset->len);
}

// Wildcard GET handler for "/*": look up req->uri in the asset table by exact
// path match. The embed table stores the SPA index at "/" (not "/index.html"),
// mirroring the old per-asset registration, so "/" matches directly.
static esp_err_t asset_wildcard_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    // Strip query string for matching (uri may contain "?..." suffix)
    char path_buf[256];
    const char *q = strchr(uri, '?');
    if (q) {
        size_t plen = (size_t)(q - uri);
        if (plen >= sizeof(path_buf)) plen = sizeof(path_buf) - 1;
        memcpy(path_buf, uri, plen);
        path_buf[plen] = '\0';
        uri = path_buf;
    }

    for (size_t i = 0; i < s_asset_count; i++) {
        if (strcmp(s_assets[i].path, uri) == 0) {
            return bb_http_serve_asset(req, &s_assets[i]);
        }
    }

    // No match → 404
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_OK;
}

bb_err_t bb_http_register_route(bb_http_handle_t server,
                                bb_http_method_t method,
                                const char *path,
                                bb_http_handler_fn handler)
{
    httpd_handle_t h = (httpd_handle_t)server;
    if (!h || !handler) return BB_ERR_INVALID_ARG;

    // /api/* routes are served by per-method wildcard handlers that dispatch
    // through bb_api_dispatch — they do NOT occupy httpd handler slots.
    if (path && strncmp(path, "/api/", 5) == 0) {
        bb_err_t derr = bb_api_dispatch_add(method, path, handler);
        if (derr != BB_OK) {
            bb_log_e(TAG, "api dispatch table full, dropping %s %s",
                     bb_http_method_str(method), path);
            // Non-fatal: return BB_OK so bb_http_register_described_route still
            // adds the OpenAPI descriptor (route is listed even if undispatchable).
        }
        return BB_OK;
    }

    // Non-/api paths (e.g. /save, captive /*, asset /*): register with httpd.
    int http_method_mapped;
    switch (method) {
        case BB_HTTP_GET:     http_method_mapped = HTTP_GET;     break;
        case BB_HTTP_POST:    http_method_mapped = HTTP_POST;    break;
        case BB_HTTP_PATCH:   http_method_mapped = HTTP_PATCH;   break;
        case BB_HTTP_PUT:     http_method_mapped = HTTP_PUT;     break;
        case BB_HTTP_DELETE:  http_method_mapped = HTTP_DELETE;  break;
        case BB_HTTP_OPTIONS: http_method_mapped = HTTP_OPTIONS; break;
        default:              return BB_ERR_INVALID_ARG;
    }

    httpd_uri_t uri = {
        .uri     = path,
        .method  = http_method_mapped,
        .handler = bb_shim_handler,
        .user_ctx = (void*)handler,
    };

    esp_err_t err = httpd_register_uri_handler(h, &uri);
    if (err != ESP_OK) {
        bb_log_e(TAG, "register route failed (%s %s): %s",
                 bb_http_method_str(method), path, esp_err_to_name(err));
        return BB_ERR_INVALID_ARG;
    }
    s_registered_handlers++;
    return BB_OK;
}

bb_err_t bb_http_resp_set_status(bb_http_request_t *req, int status_code)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req) return BB_ERR_INVALID_ARG;

    // httpd_resp_set_status stores the pointer (no copy), so the string must
    // outlive the request — bb_http_status_reason returns a static literal.
    const char *status_str = bb_http_status_reason(status_code);
    if (!status_str) return BB_ERR_INVALID_ARG;
    esp_err_t err = httpd_resp_set_status(http_req, status_str);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

bb_err_t bb_http_resp_set_type(bb_http_request_t *req, const char *mime)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req || !mime) return BB_ERR_INVALID_ARG;

    esp_err_t err = httpd_resp_set_type(http_req, mime);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

bb_err_t bb_http_resp_set_header(bb_http_request_t *req, const char *key, const char *value)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req || !key || !value) return BB_ERR_INVALID_ARG;

    esp_err_t err = httpd_resp_set_hdr(http_req, key, value);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

int bb_http_req_body_len(bb_http_request_t *req)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req) return -1;
    return http_req->content_len;
}

int bb_http_req_recv(bb_http_request_t *req, char *buf, size_t buf_size)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req || !buf) return -1;

    return httpd_req_recv(http_req, buf, buf_size);
}

bb_err_t bb_http_register_assets(bb_http_handle_t server,
                                 const bb_http_asset_t *assets,
                                 size_t n)
{
    httpd_handle_t h = (httpd_handle_t)server;
    if (!h || !assets) return BB_ERR_INVALID_ARG;

    // Validate entries upfront
    for (size_t i = 0; i < n; i++) {
        if (!assets[i].path || !assets[i].mime || !assets[i].data) {
            return BB_ERR_INVALID_ARG;
        }
    }

    // Store table for the wildcard handler
    s_assets      = assets;
    s_asset_count = n;

    // Register a single "/*" GET handler (registered last so specific routes
    // registered before this call win first-match).
    httpd_uri_t uri = {
        .uri     = "/*",
        .method  = HTTP_GET,
        .handler = asset_wildcard_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(h, &uri);
    if (err != ESP_OK) {
        bb_log_e(TAG, "register route failed (GET /*): %s", esp_err_to_name(err));
        return BB_ERR_INVALID_ARG;
    }
    s_registered_handlers++;

    bb_log_i(TAG, "assets: %u files via single wildcard handler", (unsigned)n);
    return BB_OK;
}

bb_err_t bb_http_resp_sendstr(bb_http_request_t *req, const char *str)
{
    bb_err_t err = bb_http_resp_send_chunk(req, str, str ? -1 : 0);
    if (err != BB_OK) return err;
    return bb_http_resp_send_chunk(req, NULL, 0);
}

#ifdef CONFIG_BB_HTTP_RESP_SEND_CHUNK_LIVENESS_PROBE
/* Returns true if the peer has already closed the socket (FIN received or
 * RST seen). Non-blocking; treats EAGAIN/EWOULDBLOCK as "still alive". */
static bool sock_peer_closed(httpd_req_t *http_req)
{
    int fd = httpd_req_to_sockfd(http_req);
    if (fd < 0) return true;
    char probe;
    int n = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return true;  /* clean FIN */
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return true;
    return false;
}
#endif

bb_err_t bb_http_resp_send_chunk(bb_http_request_t *req, const char *buf, int len)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req) return BB_ERR_INVALID_ARG;
#ifdef CONFIG_BB_HTTP_RESP_SEND_CHUNK_LIVENESS_PROBE
    /* Short-circuit if the peer is already gone. Doesn't fully close the
     * race (RST can land between probe and lwip_send) but stops the long
     * tail of writing N more chunks into a dead socket — particularly
     * relevant for SSE streamers (bb_log_routes, bb_event_routes) and the
     * chunked fallback path in bb_http_resp_send_json. The zero-length
     * terminator (len==0) is allowed through so callers can finalize the
     * chunked response in their cleanup path without an extra branch. */
    if (len != 0 && sock_peer_closed(http_req)) {
        return BB_ERR_INVALID_STATE;
    }
#endif
    int send_len = (len < 0) ? HTTPD_RESP_USE_STRLEN : len;
    esp_err_t err = httpd_resp_send_chunk(http_req, buf, send_len);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

bb_err_t bb_http_resp_no_content(bb_http_request_t *req)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req) return BB_ERR_INVALID_ARG;

    // httpd_resp_send emits a complete, non-chunked response with
    // Content-Length: 0. A 204 framed via httpd_resp_send_chunk (chunked
    // Transfer-Encoding) is malformed and rejected by strict HTTP proxies.
    httpd_resp_set_status(http_req, "204 No Content");
    esp_err_t err = httpd_resp_send(http_req, NULL, 0);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_STATE;
}

int bb_http_req_sockfd(bb_http_request_t *req)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req) return -1;
    return httpd_req_to_sockfd(http_req);
}

bb_err_t bb_http_req_query_key_value(bb_http_request_t *req, const char *key,
                                     char *out, size_t out_len)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req || !key || !out || out_len == 0) return BB_ERR_INVALID_ARG;

    // httpd_req_get_url_query_len returns the length without null terminator
    size_t qlen = httpd_req_get_url_query_len(http_req);
    if (qlen == 0) return BB_ERR_INVALID_ARG;

    char *query = malloc(qlen + 1);
    if (!query) return BB_ERR_NO_SPACE;

    esp_err_t err = httpd_req_get_url_query_str(http_req, query, qlen + 1);
    if (err != ESP_OK) {
        free(query);
        return BB_ERR_INVALID_ARG;
    }

    err = httpd_query_key_value(query, key, out, out_len);
    free(query);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

bool bb_http_req_query_has_key(bb_http_request_t *req, const char *key)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req || !key) return false;

    size_t qlen = httpd_req_get_url_query_len(http_req);
    if (qlen == 0) return false;

    char *query = malloc(qlen + 1);
    if (!query) return false;

    bool found = false;
    if (httpd_req_get_url_query_str(http_req, query, qlen + 1) == ESP_OK) {
        // Scan &-separated segments for `key` as a whole token (bare or key=...).
        // httpd_query_key_value can't see bare keys (no '='), so do it ourselves.
        size_t klen = strlen(key);
        const char *p = query;
        while (p && *p) {
            const char *amp = strchr(p, '&');
            size_t seg = amp ? (size_t)(amp - p) : strlen(p);
            if (seg >= klen && strncmp(p, key, klen) == 0 &&
                (seg == klen || p[klen] == '=')) {
                found = true;
                break;
            }
            p = amp ? amp + 1 : NULL;
        }
    }
    free(query);
    return found;
}

bb_err_t bb_http_req_async_handler_begin(bb_http_request_t *req,
                                         bb_http_request_t **out_async_req)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req || !out_async_req) return BB_ERR_INVALID_ARG;
    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(http_req, &async_req);
    if (err != ESP_OK) return BB_ERR_INVALID_ARG;
    *out_async_req = (bb_http_request_t *)async_req;
    return BB_OK;
}

bb_err_t bb_http_req_async_handler_complete(bb_http_request_t *async_req)
{
    httpd_req_t *http_req = (httpd_req_t *)async_req;
    if (!http_req) return BB_ERR_INVALID_ARG;
    esp_err_t err = httpd_req_async_handler_complete(http_req);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

bb_err_t bb_http_unregister_route(bb_http_handle_t server,
                                  bb_http_method_t method,
                                  const char *path)
{
    httpd_handle_t h = (httpd_handle_t)server;
    if (!h || !path) return BB_ERR_INVALID_ARG;

    int http_method;
    switch (method) {
        case BB_HTTP_GET:     http_method = HTTP_GET;     break;
        case BB_HTTP_POST:    http_method = HTTP_POST;    break;
        case BB_HTTP_PATCH:   http_method = HTTP_PATCH;   break;
        case BB_HTTP_PUT:     http_method = HTTP_PUT;     break;
        case BB_HTTP_DELETE:  http_method = HTTP_DELETE;  break;
        case BB_HTTP_OPTIONS: http_method = HTTP_OPTIONS; break;
        default: return BB_ERR_INVALID_ARG;
    }

    esp_err_t err = httpd_unregister_uri_handler(h, path, http_method);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

// No-op on ESP-IDF; service loop runs in httpd task
void bb_http_server_poll(void)
{
    // ESP-IDF httpd runs on its own FreeRTOS task; nothing to do here
}

size_t bb_http_route_handler_count(void)
{
    return (size_t)s_registered_handlers;
}

size_t bb_http_route_handler_cap(void)
{
    return (size_t)s_max_uri_handlers;
}

// ---------------------------------------------------------------------------
// JSON streaming
// ---------------------------------------------------------------------------

// Context for walking JSON children
struct json_walk_ctx {
    bb_http_request_t *req;
    bb_json_kind_t kind;
    bool is_first;
    bb_err_t err;
};

// Forward decl: streams an arbitrary node, recursing into containers so a
// single large subtree never has to be materialized as one cJSON_Print buffer.
static bb_err_t json_stream_node(bb_http_request_t *req, bb_json_t node);

static void json_child_emit(const char *key, bb_json_t child, void *ctx)
{
    struct json_walk_ctx *walk_ctx = (struct json_walk_ctx *)ctx;
    if (walk_ctx->err != BB_OK) return;

    // Emit comma before all but the first child
    if (!walk_ctx->is_first) {
        bb_err_t err = bb_http_resp_send_chunk(walk_ctx->req, ",", -1);
        if (err != BB_OK) {
            walk_ctx->err = err;
            return;
        }
    }
    walk_ctx->is_first = false;

    // For objects, emit "key":
    if (walk_ctx->kind == BB_JSON_KIND_OBJECT && key) {
        bb_err_t err = bb_http_resp_send_chunk(walk_ctx->req, "\"", -1);
        if (err != BB_OK) {
            walk_ctx->err = err;
            return;
        }
        err = bb_http_resp_send_chunk(walk_ctx->req, key, -1);
        if (err != BB_OK) {
            walk_ctx->err = err;
            return;
        }
        err = bb_http_resp_send_chunk(walk_ctx->req, "\":", -1);
        if (err != BB_OK) {
            walk_ctx->err = err;
            return;
        }
    }

    walk_ctx->err = json_stream_node(walk_ctx->req, child);
}

// Stream any JSON node. Recurses for objects/arrays so deep subtrees don't
// require a single contiguous serialization buffer (cJSON_PrintUnformatted
// can return NULL on large nested docs even with plenty of free heap, which
// previously produced malformed output like `"paths":}`). For scalars,
// fall back to literal "null" if serialize fails so the stream stays valid.
static bb_err_t json_stream_node(bb_http_request_t *req, bb_json_t node)
{
    bb_json_kind_t kind = bb_json_get_kind(node);

    if (kind == BB_JSON_KIND_OBJECT || kind == BB_JSON_KIND_ARRAY) {
        const char *open_b  = (kind == BB_JSON_KIND_OBJECT) ? "{" : "[";
        const char *close_b = (kind == BB_JSON_KIND_OBJECT) ? "}" : "]";

        bb_err_t err = bb_http_resp_send_chunk(req, open_b, -1);
        if (err != BB_OK) return err;

        struct json_walk_ctx inner = {
            .req      = req,
            .kind     = kind,
            .is_first = true,
            .err      = BB_OK,
        };
        bb_json_walk_children(node, json_child_emit, &inner);
        if (inner.err != BB_OK) return inner.err;

        return bb_http_resp_send_chunk(req, close_b, -1);
    }

    // Scalar / null — these are small; one-shot serialize is fine.
    char *s = bb_json_item_serialize(node);
    if (!s) {
        return bb_http_resp_send_chunk(req, "null", -1);
    }
    bb_err_t err = bb_http_resp_send_chunk(req, s, -1);
    bb_json_free_str(s);
    return err;
}

// ============================================================================
// STREAMING JSON ARRAY API — ESP-IDF backend
// ============================================================================

bb_err_t bb_http_resp_json_arr_begin(bb_http_request_t *req,
                                     bb_http_json_stream_t *out)
{
    if (!req || !out) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_resp_set_type(req, "application/json");
    if (err != BB_OK) return err;

    err = bb_http_resp_send_chunk(req, "[", -1);
    if (err != BB_OK) return err;

    out->_req = req;
    out->_err = BB_OK;
    out->_first = 1;
    out->_open = 1;
    return BB_OK;
}

bb_err_t bb_http_resp_json_arr_emit(bb_http_json_stream_t *stream,
                                    bb_json_t item)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;  // sticky error is a no-op

    bb_http_request_t *req = (bb_http_request_t *)stream->_req;

    // Emit comma before all but the first element
    if (!stream->_first) {
        bb_err_t err = bb_http_resp_send_chunk(req, ",", -1);
        if (err != BB_OK) {
            stream->_err = err;
            return err;
        }
    }
    stream->_first = 0;

    // Serialize and emit the item
    char *json_str = bb_json_item_serialize(item);
    bb_err_t err;
    if (json_str) {
        err = bb_http_resp_send_chunk(req, json_str, -1);
        bb_json_free_str(json_str);
    } else {
        // If serialize fails, emit literal null to keep stream valid
        err = bb_http_resp_send_chunk(req, "null", -1);
    }

    if (err != BB_OK) {
        stream->_err = err;
    }
    return err;
}

bb_err_t bb_http_resp_json_arr_end(bb_http_json_stream_t *stream)
{
    if (!stream) return BB_ERR_INVALID_ARG;

    stream->_open = 0;
    bb_http_request_t *req = (bb_http_request_t *)stream->_req;

    // Emit closing bracket
    bb_err_t err = bb_http_resp_send_chunk(req, "]", -1);
    if (err != BB_OK && stream->_err == BB_OK) {
        stream->_err = err;
    }

    // Terminate the chunked response
    bb_err_t fin = bb_http_resp_send_chunk(req, NULL, 0);
    if (fin != BB_OK && stream->_err == BB_OK) {
        stream->_err = fin;
    }

    return stream->_err;
}
