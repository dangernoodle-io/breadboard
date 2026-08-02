#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_status.h"
#include "bb_http_query.h"
#include "bb_http_prov_gate.h"
#include "bb_http_core_steer.h"
#include "bb_mem.h"
#include "bb_wdt.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "bb_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/socket.h>
#include "lwip/sockets.h"
#include "sdkconfig.h"

#if !CONFIG_LWIP_SO_LINGER
#warning "bb_http_req_async_abort's SO_LINGER setsockopt is inert without CONFIG_LWIP_SO_LINGER=y — SSE peer-abort teardown falls back to graceful FIN close instead of an immediate RST. Set CONFIG_LWIP_SO_LINGER=y to get RST-based abort teardown."
#endif

// Config bridge (B1-1364 PR5, see CLAUDE.md "Config bridge"): CONFIG_BB_HTTP_
// TASK_CORE_ID -> BB_HTTP_TASK_CORE_ID with a C default of -1 (== bb_wdt's
// BB_WDT_CORE_ANY sentinel -- "no explicit pin"), matching the Kconfig
// default. sdkconfig.h always defines the CONFIG_ symbol on a real ESP-IDF
// build (Kconfig default -1), so the #ifdef branch below never actually
// falls through to the literal default in practice -- kept anyway per the
// bridge convention, and so this .c file compiles standalone.
#ifdef CONFIG_BB_HTTP_TASK_CORE_ID
#define BB_HTTP_TASK_CORE_ID CONFIG_BB_HTTP_TASK_CORE_ID
#else
#define BB_HTTP_TASK_CORE_ID (-1)
#endif

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;
static int s_max_uri_handlers = 0;        // set by ensure_started
static int s_registered_handlers = 0;     // incremented on every successful register

/* No-op handler for ESP_HTTP_SERVER_EVENT (B1-306).
 * esp_http_server posts ESP_HTTP_SERVER_EVENT to the default event loop on every
 * HTTP event (connect, header, sent-data, etc.). Without a registered handler,
 * esp_event logs "no handlers have been registered for event ESP_HTTP_SERVER_EVENT:N"
 * at DEBUG. With an SSE viewer (/api/events) open this self-amplifies: each streamed
 * line triggers another SENT_DATA event → another DEBUG log → streamed again, causing
 * thousands of log lines/sec. Registering this no-op suppresses that spam. */
static void bb_http_noop_event_handler(void *arg, esp_event_base_t base,
                                       int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
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

// Reverse of bb_http_register_route's method mapping: httpd's method integer
// -> bb_http_method_t. Shared by every dispatch entry point that needs to
// resolve the requesting method for a bb_http_prov_gate_check() call
// (api_dispatch_handler, bb_shim_handler, method_not_allowed_err_handler) —
// asset_wildcard_handler doesn't need it, it's GET-only by registration.
// Returns false for a method no route ever registers on purpose (e.g. HEAD)
// — unlike the other three call sites, method_not_allowed_err_handler CAN
// see one of these for real: httpd's 405 error handler fires for ANY
// method against ANY URI, not just registered ones, so a genuine HEAD
// request reaches it and must be handled explicitly (see that handler's own
// unmappable-method branch) rather than treated as defensive/unreachable.
// Cross-reference: bb_http_host_method_not_allowed()'s `mapped` bool
// (bb_http_host.c) is caller-supplied rather than derived from a host
// mirror of this switch — editing the method list here does not get caught
// by any host test. Check that call site's own comment when adding/removing
// a case.
static bool bb_http_method_from_httpd(int http_method, bb_http_method_t *out)
{
    switch (http_method) {
        case HTTP_GET:     *out = BB_HTTP_GET;     return true;
        case HTTP_POST:    *out = BB_HTTP_POST;    return true;
        case HTTP_PUT:     *out = BB_HTTP_PUT;     return true;
        case HTTP_PATCH:   *out = BB_HTTP_PATCH;   return true;
        case HTTP_DELETE:  *out = BB_HTTP_DELETE;  return true;
        case HTTP_OPTIONS: *out = BB_HTTP_OPTIONS; return true;
        default:           return false;
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
    config.stack_size =
#ifdef CONFIG_BB_HTTP_TASK_STACK_SIZE
        CONFIG_BB_HTTP_TASK_STACK_SIZE;
#else
        6144;
#endif
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

    // B1-1364 PR5: steer the httpd worker off any core another component has
    // EXCLUSIVELY claimed (bb_wdt_claim_core_exclusive(), see bb_wdt.h) --
    // the second failure mode in KB 350 (httpd worker starvation; the first,
    // idle-check polarity, is handled by the core-claim/reconfigure work
    // already landed in #1192/#1197). An explicit CONFIG_BB_HTTP_TASK_CORE_ID
    // pin always wins over steering (bb_wdt_steer_core()'s own contract).
    // With nothing claimed and no explicit pin (the default), this resolves
    // to tskNO_AFFINITY unchanged -- identical to the pre-PR5 behavior of
    // leaving HTTPD_DEFAULT_CONFIG()'s core_id untouched.
    config.core_id = (BaseType_t)bb_http_core_steer_resolve(
        BB_HTTP_TASK_CORE_ID, bb_wdt_claimed_core_mask(),
        configNUMBER_OF_CORES, (int32_t)tskNO_AFFINITY);

    // max_uri_handlers is now a single constant knob (BB_HTTP_MAX_URI_HANDLERS,
    // default 12). /api/* routes do NOT consume httpd handler slots — they are
    // dispatched via the bb_dispatch_api table instead. The 12-slot default
    // covers: GET/POST/PUT/PATCH/DELETE /api/* (5) + OPTIONS /* (1) + GET /* (1)
    // + headroom for non-/api routes (/save, captive /*) registered by consumers.
    // (B1-1321: the vestigial bb_http_reserve_routes() mechanism that used to
    // track a route-count total here has been removed entirely.)
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
    // DEBUG — especially when an SSE viewer (/api/events) is open (self-amplifying loop).
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

    // Register per-method /api/* wildcards that dispatch via bb_dispatch_api.
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
// Provisioning-gate note: this handler fires on a genuine method mismatch —
// a URI IS registered, just not for req->method. Left unguarded, a
// non-allowlisted route would still leak "this URI exists" via 405 for a
// wrong-method request during provisioning, even though the correct-method
// request would 404 through bb_shim_handler's own gate check below. Gate
// here too so a denied (method,uri) reads as plain 404 regardless of which
// method the caller guesses.
//
// Unmappable-method note: bb_http_method_from_httpd() covers only
// GET/POST/PUT/PATCH/DELETE/OPTIONS. A method it can't map (e.g. HEAD, sent
// by browsers and `curl -I`) has no bb_http_method_t to run through the
// allowlist at all, so the check above is skipped for it — but that must
// NOT mean "unguarded": falling through to the plain registration check
// below would still 405 an unmappable-method request for a registered URI,
// reproducing exactly the leak this handler exists to close. While
// provisioning is active, an unmappable method fails closed (404)
// unconditionally instead.
static esp_err_t method_not_allowed_err_handler(httpd_req_t *req,
                                                httpd_err_code_t err)
{
    (void)err;
    bb_http_method_t method;
    if (bb_http_method_from_httpd(req->method, &method)) {
        if (!bb_http_prov_gate_check(method, req->uri)) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
            return ESP_OK;
        }
    } else if (bb_http_prov_gate_is_active()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_OK;
    }
    if (!bb_http_uri_is_registered(req->uri)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    } else {
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }
    return ESP_OK;
}

// Wildcard handler for GET/POST/PUT/PATCH/DELETE /api/*.
// Maps the httpd method integer back to bb_http_method_t, strips the query
// string from req->uri, then looks up in bb_dispatch_api.
//   HIT             → wrap req as bb_http_request_t* and call the handler.
//   METHOD_MISMATCH → 405 with JSON body.
//   MISS            → 404 with JSON body.
// SSE and async handlers work unchanged: their fn(req) calls
// bb_http_req_async_handler_begin + task-spawn internally.
static esp_err_t api_dispatch_handler(httpd_req_t *req)
{
    // Map httpd method integer → bb_http_method_t.
    bb_http_method_t method;
    if (!bb_http_method_from_httpd(req->method, &method)) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"method not allowed\"}");
        return ESP_OK;
    }

    // Strip query string: dispatch lookup accepts URI with query but doing it
    // here avoids duplicate work across many handlers.
    const char *uri = req->uri;

    // Provisioning gate — insertion point 1/3. During provisioning, deny
    // anything not explicitly allowlisted with the same 404 shape as a
    // genuine MISS below, so a denied route is indistinguishable from one
    // that doesn't exist (see bb_http_prov_gate.h).
    if (!bb_http_prov_gate_check(method, uri)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"not found\"}");
        return ESP_OK;
    }

    bb_http_handler_fn handler = NULL;
    bb_dispatch_api_result_t res = bb_dispatch_api_lookup(method, uri, &handler);

    if (res == BB_DISPATCH_API_HIT) {
        if (!handler) {
            // Defensive: bb_dispatch_api_add is never fed a NULL handler (see
            // bb_http_register_route / bb_http_register_described_route), so
            // this should be unreachable. Guard anyway rather than risk a
            // null-deref if that invariant is ever broken.
            httpd_resp_set_status(req, "501 Not Implemented");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"no handler registered\"}");
            return ESP_OK;
        }
        // Wrap httpd_req_t* as bb_http_request_t* — identical cast used by
        // bb_shim_handler. The fn(req) contract is the same for sync and async
        // (SSE/async) handlers.
        bb_err_t herr = handler((bb_http_request_t *)req);
        return herr == BB_OK ? ESP_OK : ESP_FAIL;
    }

    if (res == BB_DISPATCH_API_METHOD_MISMATCH) {
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
// bbtool:init composition entry points (codegen linchpin)
// ============================================================================
//
// Anything that used to live in bb_init_init() and referenced the HTTP
// server is wired via `// bbtool:init` markers in bb_http_server.h instead:
//
//   1. HTTP autostart, pre_http-tier, provides=http_server — THE codegen
//      linchpin: returns the live server handle (captured via __auto_type
//      by the generated bb_app_init_rest()) instead of a bb_err_t, so
//      every server=true regular-tier entry receives it as an argument.
//   2. The route-registry-cap audit, regular-tier, server=true, so it runs
//      after every real route registration and receives the handle.

// When CONFIG_BB_HTTP_AUTOSTART is on (default), starts the HTTP server on
// first call (idempotent — reuses the existing s_server if already started
// by some other path). When off, this is a no-op that just returns whatever
// handle already exists (NULL if nothing has started the server yet — the
// consumer is expected to call bb_http_server_start() itself; per wire.py's
// documented limitation, a NULL handle here surfaces as a runtime issue in
// downstream server=true consumers, not a codegen-time error).
bb_http_handle_t bb_http_autostart_init(void)
{
#if defined(CONFIG_BB_HTTP_AUTOSTART) && CONFIG_BB_HTTP_AUTOSTART
    if (!bb_http_server_get_handle()) {
        esp_err_t err = bb_http_server_start();
        if (err != ESP_OK) {
            bb_log_e(TAG, "autostart: failed to start HTTP server: %s", esp_err_to_name(err));
        }
    }
#endif
    return bb_http_server_get_handle();
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

    // Provisioning gate — insertion point 2/3. Every non-/api imperative
    // route registered via bb_http_register_route (e.g. /ping, /save)
    // dispatches through this one shim, so gating here covers all of them
    // in one place. Denied requests never reach the consumer's handler.
    bb_http_method_t method;
    if (bb_http_method_from_httpd(req->method, &method) &&
        !bb_http_prov_gate_check(method, req->uri)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_OK;
    }

    return fn((bb_http_request_t*)req) == BB_OK ? ESP_OK : ESP_FAIL;
}

// Asset table used by the single wildcard handler.
static const bb_http_asset_t *s_assets        = NULL;
static size_t                 s_asset_count   = 0;
// No-match fallback registered via bb_http_register_assets_with_fallback
// (e.g. bb_wifi_prov's captive-portal redirect). NULL means "no fallback" —
// asset_wildcard_handler keeps the plain 404 behavior.
static bb_http_handler_fn     s_asset_fallback = NULL;

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
            // Provisioning gate — insertion point 3/3. This handler is
            // registered directly as "/*"'s .handler (see
            // bb_http_register_assets_with_fallback below) and does NOT go
            // through bb_shim_handler, so it needs its own gate check — this
            // is exactly the hole the design work behind this PR flagged:
            // gating only api_dispatch_handler + bb_shim_handler would have
            // left every asset request ungated.
            //
            // Gated ONLY on an asset-table HIT, deliberately — NOT
            // unconditionally at the top of this handler. A MISS falls
            // through to the registered fallback below (e.g. bb_wifi_prov's
            // captive-portal redirect) ungated: client OSes probe
            // unenumerable paths (/generate_204, /hotspot-detect.html, …)
            // to auto-detect the captive portal, and bb_http_prov_gate's
            // wildcard allowlist entries are restricted to /api/ paths (see
            // bb_http_prov_gate.h), so those probe paths could never be
            // allowlisted directly. The fallback itself is
            // provisioning-required (it's what makes portal auto-popup
            // work), so "no asset match -> registered fallback" is treated
            // as an allowed outcome rather than gated here.
            if (!bb_http_prov_gate_check(BB_HTTP_GET, req->uri)) {
                httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
                return ESP_OK;
            }
            return bb_http_serve_asset(req, &s_assets[i]);
        }
    }

    // No match: hand off to a registered fallback (e.g. bb_wifi_prov's
    // captive-portal redirect) using the same bb_shim_handler bridging
    // contract — cast req to bb_http_request_t* and translate BB_OK/error to
    // ESP_OK/ESP_FAIL. Falls through to the plain 404 when no fallback is
    // registered. Deliberately NOT gated — see the comment above.
    //
    // INVARIANT this relies on: s_asset_fallback can only be non-NULL while
    // the gate can be active because the only in-tree non-NULL-fallback
    // caller is bb_wifi_prov_start() (bb_wifi_prov.c), which commits its
    // fallback only after ITS OWN "/*" registration wins (see
    // bb_http_register_assets_with_fallback below, which commits state only
    // post-success) — so a live fallback here means bb_wifi_prov owns this
    // wildcard. See bb_http_register_assets_with_fallback's own comment for
    // the obligation this places on any future non-NULL-fallback caller.
    if (s_asset_fallback) {
        return s_asset_fallback((bb_http_request_t*)req) == BB_OK ? ESP_OK : ESP_FAIL;
    }

    // No match, no fallback → 404
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
    // through bb_dispatch_api — they do NOT occupy httpd handler slots.
    if (path && strncmp(path, "/api/", 5) == 0) {
        bb_err_t derr = bb_dispatch_api_add(method, path, handler);
        if (derr == BB_ERR_INVALID_STATE) {
            // Duplicate (method,path): bb_dispatch_api_add already logged
            // (bb_log_w, "first registration wins") — this is the
            // intentionally-tolerated case where two independently-composed
            // components register the same /api/* path (e.g. bb_wifi_prov and
            // bb_wifi_http both wanting POST /api/wifi/scan). Swallow and
            // return BB_OK: a caller that doesn't special-case duplicates
            // must not be broken by this; bb_http_register_described_route
            // still adds the OpenAPI descriptor either way. A caller that
            // wants to distinguish "lost the dup race" from "genuinely
            // failed" would need to call bb_dispatch_api_add directly instead
            // of going through this function — no caller does that today;
            // bb_wifi_prov.c's BB_ERR_INVALID_STATE branch is defensive/dead
            // for exactly this reason (see its own comment).
            return BB_OK;
        }
        if (derr != BB_OK) {
            // Genuine failure (e.g. BB_ERR_NO_SPACE — dispatch table full):
            // propagate so the caller can detect and react to the drop
            // instead of silently shipping a dead /api/* endpoint.
            bb_log_e(TAG, "api dispatch table add failed, dropping %s %s: %d",
                     bb_http_method_str(method), path, (int)derr);
            return derr;
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
    // outlive the request. bb_http_status_reason returns a static literal for
    // a tabled code; an untabled code (B1-954) falls back through
    // bb_http_status_line to a numerically-correct generic line, formatted
    // into a task-local buffer. Every call site runs on exactly one FreeRTOS
    // task at a time (the httpd control task pre-handoff, or a single async
    // handler task post-handoff via bb_http_req_async_handler_begin — e.g.
    // bb_event_routes SSE), and execution within a task is serial, so a
    // __thread buffer needs no synchronization and has no wraparound hazard,
    // unlike a shared pool sized to a guessed constant. __thread is C11 and
    // supported by ESP-IDF's FreeRTOS port on both Xtensa and RISC-V
    // (including esp32c3); storage is allocated on every task's own stack —
    // measured via readelf on both ports: uxInitialiseStackTLS() rounds the
    // 24-byte variable up to a 32-byte-per-task tax, paid by EVERY task in
    // the system, including tasks that never touch HTTP. This is currently
    // the only __thread variable in the tree; a second one anywhere adds to
    // the same per-task total, so re-run this measurement if one is added.
    static __thread char s_fallback_buf[24];
    const char *status_str = bb_http_status_line(status_code, s_fallback_buf,
                                                  sizeof(s_fallback_buf));
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

bb_err_t bb_http_req_get_header(bb_http_request_t *req, const char *name,
                                char *out, size_t out_len)
{
    httpd_req_t *http_req = (httpd_req_t*)req;
    if (!http_req || !name || !out || out_len == 0U) return BB_ERR_INVALID_ARG;

    out[0] = '\0';

    size_t hdr_len = httpd_req_get_hdr_value_len(http_req, name);
    if (hdr_len == 0U) return BB_ERR_NOT_FOUND;

    esp_err_t err = httpd_req_get_hdr_value_str(http_req, name, out, out_len);
    if (err == ESP_OK) return BB_OK;
    if (err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        // Header present but longer than out_len-1: value copied is still
        // usable (bounded, truncated) — that is BB_OK for our callers (e.g.
        // a truncated User-Agent is fine). Ensure explicit NUL-termination.
        out[out_len - 1] = '\0';
        return BB_OK;
    }
    out[0] = '\0';
    return BB_ERR_NOT_FOUND;
}

// WARNING for any future non-NULL-fallback caller: asset_wildcard_handler's
// no-match branch dispatches to `fallback` UNGATED (see that handler's own
// comment). That is safe today only because the sole caller,
// bb_wifi_prov_start(), is unreachable-while-ungated by construction — a new
// caller passing non-NULL here must not be reachable while
// bb_http_prov_gate can be active, or it reopens the hole the gate exists to
// close.
bb_err_t bb_http_register_assets_with_fallback(bb_http_handle_t server,
                                               const bb_http_asset_t *assets,
                                               size_t n,
                                               bb_http_handler_fn fallback)
{
    httpd_handle_t h = (httpd_handle_t)server;
    if (!h) return BB_ERR_INVALID_ARG;
    // assets/n must agree: either a real table (assets != NULL, n may be 0
    // for an empty table) or "no assets at all" (assets == NULL, n == 0 —
    // the fallback-only case, e.g. a captive portal with no caller-supplied
    // UI). assets == NULL with n > 0 is a mismatched/invalid call.
    if (!assets && n > 0) return BB_ERR_INVALID_ARG;

    // Validate entries upfront
    for (size_t i = 0; i < n; i++) {
        if (!assets[i].path || !assets[i].mime || !assets[i].data) {
            return BB_ERR_INVALID_ARG;
        }
    }

    // Register a single "/*" GET handler (registered last so specific routes
    // registered before this call win first-match). Validate + register
    // BEFORE touching the file-scope state: on failure the previously
    // registered wildcard handler (if any) must keep dispatching against its
    // OLD table/fallback untouched, not a half-applied new one.
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

    // Registration succeeded — now safe to commit the new table + fallback
    // for the wildcard handler to read.
    s_assets         = assets;
    s_asset_count    = n;
    s_asset_fallback = fallback;

    bb_log_i(TAG, "assets: %u files via single wildcard handler%s", (unsigned)n,
             fallback ? " (with no-match fallback)" : "");
    return BB_OK;
}

bb_err_t bb_http_register_assets(bb_http_handle_t server,
                                 const bb_http_asset_t *assets,
                                 size_t n)
{
    // Back-compat: assets is required (unlike the _with_fallback entry,
    // which additionally allows a fallback-only NULL-assets registration).
    if (!assets) return BB_ERR_INVALID_ARG;
    return bb_http_register_assets_with_fallback(server, assets, n, NULL);
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
     * relevant for SSE streamers (bb_event_routes) and other chunked
     * response paths. The zero-length terminator (len==0) is allowed
     * through so callers can finalize the chunked response in their
     * cleanup path without an extra branch. */
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

bb_err_t bb_http_req_uri(bb_http_request_t *req, char *out, size_t out_cap)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req || !out || out_cap == 0) return BB_ERR_INVALID_ARG;

    // req->uri holds the path (plus any query string) — strip the query
    // string to match the dispatch-lookup contract (mirrors
    // api_dispatch_handler's own stripping).
    return bb_http_uri_strip_query_copy(http_req->uri, out, out_cap);
}

bb_err_t bb_http_req_query_key_value(bb_http_request_t *req, const char *key,
                                     char *out, size_t out_len)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req || !key || !out || out_len == 0) return BB_ERR_INVALID_ARG;

    // httpd_req_get_url_query_len returns the length without null terminator
    size_t qlen = httpd_req_get_url_query_len(http_req);
    if (qlen == 0) return BB_ERR_INVALID_ARG;

    char *query = bb_malloc_prefer_spiram(qlen + 1);
    if (!query) return BB_ERR_NO_SPACE;

    esp_err_t err = httpd_req_get_url_query_str(http_req, query, qlen + 1);
    if (err != ESP_OK) {
        bb_mem_free(query);
        return BB_ERR_INVALID_ARG;
    }

    err = httpd_query_key_value(query, key, out, out_len);
    bb_mem_free(query);
    return err == ESP_OK ? BB_OK : BB_ERR_INVALID_ARG;
}

bool bb_http_req_query_has_key(bb_http_request_t *req, const char *key)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req || !key) return false;

    size_t qlen = httpd_req_get_url_query_len(http_req);
    if (qlen == 0) return false;

    char *query = bb_malloc_prefer_spiram(qlen + 1);
    if (!query) return false;

    bool found = false;
    if (httpd_req_get_url_query_str(http_req, query, qlen + 1) == ESP_OK) {
        found = bb_http_query_token_present(query, key);
    }
    bb_mem_free(query);
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

bool bb_http_req_peer_alive(bb_http_request_t *req)
{
    httpd_req_t *http_req = (httpd_req_t *)req;
    if (!http_req) return false;
    int fd = bb_http_req_sockfd(req);
    if (fd < 0) return false;

    // Peek without consuming. recv()==0 means peer sent FIN; recv()==-1 with
    // EAGAIN/EWOULDBLOCK means alive but quiet; recv()==-1 with anything else
    // (ECONNRESET, EBADF, ENOTCONN) is dead.
    char peek;
    ssize_t n = recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return false;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
    return true;
}

bb_err_t bb_http_req_async_abort(bb_http_request_t *async_req)
{
    httpd_req_t *http_req = (httpd_req_t *)async_req;
    if (!http_req) return BB_ERR_INVALID_ARG;

    // Arm SO_LINGER{on,0} before releasing the async request so the socket's
    // eventual close (below, and any close httpd performs afterward) triggers
    // a RST on httpd's next session-cleanup pass (typically sub-second)
    // instead of a graceful FIN exchange that would otherwise park the PCB in
    // CLOSE_WAIT until this side later closes it (B1-517). Requires
    // CONFIG_LWIP_SO_LINGER=y; if disabled, the setsockopt fails harmlessly
    // and we fall back to the pre-existing graceful-close behavior — logged
    // (bb_log_w), non-fatal.
    int fd = bb_http_req_sockfd(async_req);
    if (fd >= 0) {
        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) != 0) {
            bb_log_w(TAG, "async_abort: SO_LINGER set failed on fd=%d errno=%d", fd, errno);
        }
    }

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

