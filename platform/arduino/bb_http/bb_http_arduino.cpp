#ifdef ARDUINO

#include "bb_http.h"
#include <Arduino.h>
#include <Adafruit_CC3000.h>
#include <Adafruit_CC3000_Server.h>
#include <string.h>

// Maximum number of registered routes (tune per-target; AVR Uno has 2KB SRAM)
#ifndef BB_HTTP_MAX_ROUTES
#define BB_HTTP_MAX_ROUTES 2
#endif

// Server socket constants
#define HTTP_PORT 80

// Buffer sizes for request/response
#define METHOD_BUFLEN 6
#define PATH_BUFLEN 16
#define BODY_BUFLEN 0
#define LINE_BUFLEN 40

// Global state. NOTE: Adafruit_CC3000_Server is intentionally NOT declared at
// file scope — its constructor would run during C runtime init, before
// cc3000.begin(), and corrupts CC3000 SPI state. Instead it lives as a
// function-static inside bb_http_server_poll() so it is constructed from
// loop() context (matches Phase A6 pattern that works reliably).
static struct {
    bool started;
} g_server = {false};

// Route table
static struct {
    bb_http_method_t method;
    char path[PATH_BUFLEN];
    bb_http_handler_fn handler;
} g_routes[BB_HTTP_MAX_ROUTES];
static int g_route_count = 0;

// Response buffer: full status line + headers + body accumulate here before
// a single client.write(). CC3000's fastrprint() flush behavior is unreliable
// across many tiny writes, so we batch into one send.
#define RESP_BUFLEN 96

// Request object (allocated on stack in poll loop)
typedef struct {
    Adafruit_CC3000_ClientRef *client;
    char method[METHOD_BUFLEN];
    char path[PATH_BUFLEN];
    int resp_status;
    bool status_written;
    bool headers_ended;
    uint8_t resp_len;
    char resp[RESP_BUFLEN];
} bb_http_request_impl_t;

// Append to response buffer, truncating silently if overflow.
static void resp_append(bb_http_request_impl_t *r, const char *s, size_t n)
{
    if (!r || !s) return;
    size_t room = (r->resp_len < RESP_BUFLEN) ? (RESP_BUFLEN - r->resp_len) : 0;
    if (n > room) n = room;
    memcpy(r->resp + r->resp_len, s, n);
    r->resp_len += n;
}

static void resp_append_str(bb_http_request_impl_t *r, const char *s)
{
    if (s) resp_append(r, s, strlen(s));
}


// extern "C" for C callers (the header uses extern "C" guard)
extern "C" {

bb_err_t bb_http_server_start(void)
{
    if (g_server.started) {
        return BB_OK;
    }

    // server.begin() is deferred to first poll (see note on g_server)
    g_server.started = true;
    g_route_count = 0;

    return BB_OK;
}

bb_err_t bb_http_server_stop(void)
{
    if (!g_server.started) {
        return BB_OK;
    }

    g_server.started = false;
    g_route_count = 0;
    return BB_OK;
}

bb_http_handle_t bb_http_server_get_handle(void)
{
    return (bb_http_handle_t)&g_server;
}

bb_err_t bb_http_register_route(bb_http_handle_t server,
                                bb_http_method_t method,
                                const char *path,
                                bb_http_handler_fn handler)
{
    if (g_route_count >= BB_HTTP_MAX_ROUTES) {
        return BB_ERR_INVALID_ARG;
    }
    if (!path || !handler) {
        return BB_ERR_INVALID_ARG;
    }

    // Store route in table
    g_routes[g_route_count].method = method;
    strncpy(g_routes[g_route_count].path, path, PATH_BUFLEN - 1);
    g_routes[g_route_count].path[PATH_BUFLEN - 1] = '\0';
    g_routes[g_route_count].handler = handler;
    g_route_count++;

    return BB_OK;
}

// Append status line (once) and end-of-headers marker (once) to the response
// buffer. After this, the body may be appended. Idempotent.
static void flush_headers(bb_http_request_impl_t *r)
{
    if (!r) return;
    if (!r->status_written) {
        char status_line[20];
        int n = snprintf(status_line, sizeof(status_line), "HTTP/1.0 %d\r\n", r->resp_status);
        if (n > 0) resp_append(r, status_line, (size_t)n);
        r->status_written = true;
    }
    if (!r->headers_ended) {
        static const char eoh[] = "Connection: close\r\n\r\n";
        resp_append_str(r, eoh);
        r->headers_ended = true;
    }
}

// Helper: parse HTTP request line
static bool parse_request_line(const char *line, char *method, char *path)
{
    int nread = sscanf(line, "%7s %63s", method, path);
    return nread == 2;
}

// Helper: find route and call handler
static bb_err_t dispatch_request(bb_http_request_impl_t *req_impl)
{
    // Find matching route
    for (int i = 0; i < g_route_count; i++) {
        if (g_routes[i].method == BB_HTTP_GET && strcmp(req_impl->method, "GET") == 0 &&
            strcmp(g_routes[i].path, req_impl->path) == 0) {
            return g_routes[i].handler((bb_http_request_t *)req_impl);
        }
        if (g_routes[i].method == BB_HTTP_POST && strcmp(req_impl->method, "POST") == 0 &&
            strcmp(g_routes[i].path, req_impl->path) == 0) {
            return g_routes[i].handler((bb_http_request_t *)req_impl);
        }
        // PATCH, PUT, DELETE, OPTIONS not supported on Arduino MVP
    }

    // No route found → 404
    req_impl->resp_status = 404;
    return BB_OK;
}

void bb_http_server_poll(void)
{
    if (!g_server.started) {
        return;
    }

    // Function-static server: constructor runs on first loop() entry, AFTER
    // cc3000.begin() has completed. This is critical — a file-scope instance
    // corrupts CC3000 SPI state.
    static Adafruit_CC3000_Server server(HTTP_PORT);
    static bool begun = false;
    if (!begun) {
        server.begin();
        begun = true;
        Serial.println(F("listening on :80"));
    }

    // Try to accept a connection (non-blocking)
    Adafruit_CC3000_ClientRef client = server.available();
    if (!client) {
        return;
    }

    // Parse request
    bb_http_request_impl_t req_impl;
    memset(&req_impl, 0, sizeof(req_impl));
    req_impl.client = &client;
    req_impl.resp_status = 200;

    // Read request line with 5s timeout
    char line_buf[LINE_BUFLEN];
    int line_len = 0;
    unsigned long timeout = millis() + 5000;
    bool got_request_line = false;

    while (millis() < timeout) {
        if (!client.available()) {
            delay(10);
            continue;
        }

        char c = client.read();
        if (c == '\n') {
            if (line_len > 0 && line_buf[line_len - 1] == '\r') {
                line_len--;  // Strip \r
            }
            line_buf[line_len] = '\0';
            got_request_line = true;
            break;
        }
        if (c != '\r') {
            if (line_len < LINE_BUFLEN - 1) {
                line_buf[line_len++] = c;
            }
        }
    }

    if (!got_request_line) {
        client.close();
        return;
    }
    if (!parse_request_line(line_buf, req_impl.method, req_impl.path)) {
        client.close();
        return;
    }

    // Validate method and path length
    if (strlen(req_impl.method) >= METHOD_BUFLEN || strlen(req_impl.path) >= PATH_BUFLEN) {
        // 414 URI Too Long
        client.fastrprint("HTTP/1.0 414 URI Too Long\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        client.close();
        return;
    }

    // Read headers until blank line (skip body)
    timeout = millis() + 5000;
    while (millis() < timeout) {
        line_len = 0;
        bool got_line = false;

        while (millis() < timeout) {
            if (!client.available()) {
                delay(10);
                continue;
            }

            char c = client.read();
            if (c == '\n') {
                if (line_len > 0 && line_buf[line_len - 1] == '\r') {
                    line_len--;
                }
                line_buf[line_len] = '\0';
                got_line = true;
                break;
            }
            if (c != '\r') {
                if (line_len < LINE_BUFLEN - 1) {
                    line_buf[line_len++] = c;
                }
            }
        }

        if (!got_line) {
            break;
        }

        if (line_len == 0) {
            // Blank line = end of headers
            break;
        }
    }

    // Dispatch to handler
    dispatch_request(&req_impl);

    // Ensure status + headers-end are in the buffer (handler may have set
    // headers but not called resp_send).
    flush_headers(&req_impl);

    // Single write for the entire response — CC3000 is unreliable across
    // many tiny fastrprint calls.
    if (req_impl.resp_len > 0) {
        client.write((const uint8_t *)req_impl.resp, req_impl.resp_len);
    }

    client.close();
}

// Response helpers
bb_err_t bb_http_resp_set_status(bb_http_request_t *req, int status_code)
{
    bb_http_request_impl_t *r = (bb_http_request_impl_t *)req;
    if (!r) return BB_ERR_INVALID_ARG;
    r->resp_status = status_code;
    return BB_OK;
}

bb_err_t bb_http_resp_set_header(bb_http_request_t *req, const char *key, const char *value)
{
    bb_http_request_impl_t *r = (bb_http_request_impl_t *)req;
    if (!r || !key || !value) return BB_ERR_INVALID_ARG;

    // Headers can't be set after the body has been appended.
    if (r->headers_ended) return BB_ERR_INVALID_ARG;

    // Write status line on first header call (if not yet written)
    if (!r->status_written) {
        char status_line[20];
        int n = snprintf(status_line, sizeof(status_line), "HTTP/1.0 %d\r\n", r->resp_status);
        if (n > 0) resp_append(r, status_line, (size_t)n);
        r->status_written = true;
    }

    resp_append_str(r, key);
    resp_append_str(r, ": ");
    resp_append_str(r, value);
    resp_append_str(r, "\r\n");

    return BB_OK;
}

bb_err_t bb_http_resp_send(bb_http_request_t *req, const char *body, size_t len)
{
    bb_http_request_impl_t *r = (bb_http_request_impl_t *)req;
    if (!r) return BB_ERR_INVALID_ARG;

    // Ensure status line + end-of-headers are appended before body.
    flush_headers(r);

    if (body && len > 0) {
        resp_append(r, body, len);
    }

    return BB_OK;
}

bb_err_t bb_http_resp_send_err(bb_http_request_t *req, int status_code, const char *message)
{
    bb_http_request_impl_t *r = (bb_http_request_impl_t *)req;
    if (!r) return BB_ERR_INVALID_ARG;

    bb_http_resp_set_status(req, status_code);
    bb_http_resp_send(req, message, message ? strlen(message) : 0);
    return BB_OK;
}

int bb_http_req_body_len(bb_http_request_t *req)
{
    // Body not supported on Arduino MVP (aggressive RAM optimization).
    return 0;
}

int bb_http_req_recv(bb_http_request_t *req, char *buf, size_t buf_size)
{
    // Body not supported on Arduino MVP (aggressive RAM optimization).
    return 0;
}

bb_err_t bb_http_resp_sendstr(bb_http_request_t *req, const char *str)
{
    if (!str) return bb_http_resp_send(req, NULL, 0);
    return bb_http_resp_send(req, str, strlen(str));
}

bb_err_t bb_http_resp_send_chunk(bb_http_request_t *req, const char *buf, int len)
{
    // Chunked send not supported on Arduino MVP.
    return BB_ERR_INVALID_STATE;
}

int bb_http_req_sockfd(bb_http_request_t *req)
{
    // Socket fd not supported on Arduino MVP.
    return -1;
}

bb_err_t bb_http_req_query_key_value(bb_http_request_t *req, const char *key,
                                     char *out, size_t out_len)
{
    // Query string parsing not supported on Arduino MVP.
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_http_req_async_handler_begin(bb_http_request_t *req,
                                         bb_http_request_t **out_async_req)
{
    // Async handlers not supported on Arduino MVP.
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_http_req_async_handler_complete(bb_http_request_t *async_req)
{
    // Async handlers not supported on Arduino MVP.
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_http_unregister_route(bb_http_handle_t server,
                                  bb_http_method_t method,
                                  const char *path)
{
    // Route unregistration not supported on Arduino MVP.
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_http_register_assets(bb_http_handle_t server,
                                 const bb_http_asset_t *assets,
                                 size_t n)
{
    // Not implemented on Arduino MVP (aggressive RAM optimization).
    return BB_ERR_INVALID_STATE;
}

}  // extern "C"

#endif  // ARDUINO
