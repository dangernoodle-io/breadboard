// Host stubs for bb_http functions that require a real HTTP server backend.
// These are no-ops used only by the native (host) test environment.
// The route registry (route_registry.c) is portable and compiled directly.
//
// Capture harness: tests can arm a capture slot via bb_http_host_capture_begin
// before invoking a handler; the response helpers record status, content-type,
// and body into the slot instead of sending them to a real network connection.
// Only one slot is active at a time (host tests are single-threaded).
#include "bb_http.h"
#include "bb_http_host.h"
#include "bb_json.h"
#include <stdlib.h>
#include <string.h>

// Test hook: force bb_http_register_route to fail
static bool s_force_register_fail = false;
// Test hook: force bb_http_req_recv to return -1
static bool s_force_recv_fail = false;

// ============================================================================
// Capture slot (single, host tests are single-threaded)
// ============================================================================

typedef struct {
    bb_http_request_t *req;      // non-NULL when a capture is active
    int                status;   // last status set (default 200)
    char               content_type[64];
    char              *body;     // heap-owned, grows via realloc
    size_t             body_len;
    // injected request body (not owned; caller ensures lifetime)
    const char        *req_body;
    int                req_body_len;
} capture_slot_t;

static capture_slot_t s_cap;

static capture_slot_t *capture_find(bb_http_request_t *req)
{
    if (s_cap.req == req) return &s_cap;
    return NULL;
}

void bb_http_host_force_register_fail(bool fail)
{
    s_force_register_fail = fail;
}

void bb_http_host_force_recv_fail(bool fail)
{
    s_force_recv_fail = fail;
}

bb_err_t bb_http_register_route(bb_http_handle_t server,
                                bb_http_method_t method,
                                const char *path,
                                bb_http_handler_fn handler)
{
    (void)server;
    (void)method;
    (void)path;
    (void)handler;
    if (s_force_register_fail) {
        return BB_ERR_INVALID_STATE;
    }
    return BB_OK;
}

bb_err_t bb_http_resp_set_status(bb_http_request_t *req, int status_code)
{
    capture_slot_t *cap = capture_find(req);
    if (cap) {
        cap->status = status_code;
        return BB_OK;
    }
    (void)req;
    return BB_OK;
}

bb_err_t bb_http_resp_set_type(bb_http_request_t *req, const char *mime)
{
    capture_slot_t *cap = capture_find(req);
    if (cap && mime) {
        strncpy(cap->content_type, mime, sizeof(cap->content_type) - 1);
        cap->content_type[sizeof(cap->content_type) - 1] = '\0';
        return BB_OK;
    }
    (void)req;
    return BB_OK;
}

bb_err_t bb_http_resp_set_header(bb_http_request_t *req, const char *key, const char *value)
{
    (void)req;
    (void)key;
    (void)value;
    return BB_OK;
}

bb_err_t bb_http_resp_send(bb_http_request_t *req, const char *body, size_t len)
{
    capture_slot_t *cap = capture_find(req);
    if (cap && body && len > 0) {
        char *newbuf = realloc(cap->body, cap->body_len + len + 1);
        if (newbuf) {
            memcpy(newbuf + cap->body_len, body, len);
            cap->body_len += len;
            newbuf[cap->body_len] = '\0';
            cap->body = newbuf;
        }
        return BB_OK;
    }
    (void)req;
    return BB_OK;
}

bb_err_t bb_http_resp_send_err(bb_http_request_t *req, int status_code, const char *message)
{
    capture_slot_t *cap = capture_find(req);
    if (cap) {
        cap->status = status_code;
        if (message) {
            size_t len = strlen(message);
            char *newbuf = realloc(cap->body, cap->body_len + len + 1);
            if (newbuf) {
                memcpy(newbuf + cap->body_len, message, len);
                cap->body_len += len;
                newbuf[cap->body_len] = '\0';
                cap->body = newbuf;
            }
        }
        return BB_OK;
    }
    (void)req;
    (void)status_code;
    (void)message;
    return BB_OK;
}

int bb_http_req_body_len(bb_http_request_t *req)
{
    capture_slot_t *cap = capture_find(req);
    if (cap && cap->req_body) return cap->req_body_len;
    (void)req;
    return 0;
}

int bb_http_req_recv(bb_http_request_t *req, char *buf, size_t buf_size)
{
    if (s_force_recv_fail) return -1;
    capture_slot_t *cap = capture_find(req);
    if (cap && cap->req_body && cap->req_body_len > 0) {
        int n = cap->req_body_len;
        if ((size_t)n >= buf_size) n = (int)(buf_size - 1);
        memcpy(buf, cap->req_body, (size_t)n);
        buf[n] = '\0';
        return n;
    }
    (void)req;
    (void)buf;
    (void)buf_size;
    return 0;
}

void bb_http_set_cors_methods(const char *methods)   { (void)methods; }
void bb_http_set_cors_headers(const char *headers)   { (void)headers; }

bb_err_t bb_http_server_start(void)   { return BB_OK; }
bb_err_t bb_http_server_stop(void)    { return BB_OK; }
bb_http_handle_t bb_http_server_get_handle(void) { return NULL; }
void bb_http_server_poll(void) {}

bb_err_t bb_http_register_assets(bb_http_handle_t server,
                                 const bb_http_asset_t *assets,
                                 size_t n)
{
    (void)server;
    (void)assets;
    (void)n;
    return BB_OK;
}

bb_err_t bb_http_resp_sendstr(bb_http_request_t *req, const char *str)
{
    if (str) return bb_http_resp_send(req, str, strlen(str));
    return BB_OK;
}

bb_err_t bb_http_resp_send_chunk(bb_http_request_t *req, const char *buf, int len)
{
    capture_slot_t *cap = capture_find(req);
    if (cap && buf) {
        size_t add = (len < 0) ? strlen(buf) : (size_t)len;
        if (add > 0) {
            char *grown = realloc(cap->body, cap->body_len + add + 1);
            if (grown) {
                cap->body = grown;
                memcpy(cap->body + cap->body_len, buf, add);
                cap->body_len += add;
                cap->body[cap->body_len] = '\0';
            }
        }
    }
    // NULL/zero-length terminator on chunked encoding — no-op for capture.
    return BB_OK;
}

bb_err_t bb_http_resp_send_json(bb_http_request_t *req, bb_json_t doc)
{
    capture_slot_t *cap = capture_find(req);
    if (cap) {
        // Default content-type for JSON if not already set
        if (cap->content_type[0] == '\0') {
            strncpy(cap->content_type, "application/json",
                    sizeof(cap->content_type) - 1);
            cap->content_type[sizeof(cap->content_type) - 1] = '\0';
        }
        char *serialized = bb_json_serialize(doc);
        if (serialized) {
            size_t len = strlen(serialized);
            char *newbuf = realloc(cap->body, cap->body_len + len + 1);
            if (newbuf) {
                memcpy(newbuf + cap->body_len, serialized, len);
                cap->body_len += len;
                newbuf[cap->body_len] = '\0';
                cap->body = newbuf;
            }
            bb_json_free_str(serialized);
        }
        return BB_OK;
    }
    (void)req;
    (void)doc;
    return BB_OK;
}

int bb_http_req_sockfd(bb_http_request_t *req)
{
    (void)req;
    return -1;
}

bb_err_t bb_http_req_query_key_value(bb_http_request_t *req, const char *key,
                                     char *out, size_t out_len)
{
    (void)req;
    (void)key;
    (void)out;
    (void)out_len;
    return BB_ERR_INVALID_ARG;
}

bb_err_t bb_http_req_async_handler_begin(bb_http_request_t *req,
                                         bb_http_request_t **out_async_req)
{
    (void)req;
    (void)out_async_req;
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_http_req_async_handler_complete(bb_http_request_t *async_req)
{
    (void)async_req;
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_http_unregister_route(bb_http_handle_t server,
                                  bb_http_method_t method,
                                  const char *path)
{
    (void)server;
    (void)method;
    (void)path;
    return BB_OK;
}

bb_err_t bb_http_server_ensure_started(void) { return BB_OK; }

size_t bb_http_route_handler_count(void) { return 0; }
size_t bb_http_route_handler_cap(void)   { return 0; }

static int s_host_reserved_routes = 0;
void bb_http_reserve_routes(int n) { if (n > 0) s_host_reserved_routes += n; }
int  bb_http_host_reserved_routes(void) { return s_host_reserved_routes; }
void bb_http_host_reset_reserved(void)  { s_host_reserved_routes = 0; }

// ============================================================================
// Public capture API
// ============================================================================

// Sentinel: a small non-NULL non-stack address used as the fake req cookie.
// We store its address so it is stable across calls.
static int s_capture_cookie;

void bb_http_host_capture_begin(bb_http_request_t **out_req)
{
    bb_http_request_t *req = (bb_http_request_t *)&s_capture_cookie;
    s_cap.req          = req;
    s_cap.status       = 200;
    s_cap.body         = NULL;
    s_cap.body_len     = 0;
    s_cap.req_body     = NULL;
    s_cap.req_body_len = 0;
    memset(s_cap.content_type, 0, sizeof(s_cap.content_type));
    if (out_req) *out_req = req;
}

void bb_http_host_capture_set_req_body(const char *body, int len)
{
    s_cap.req_body     = body;
    s_cap.req_body_len = body ? len : 0;
}

bb_err_t bb_http_host_capture_end(bb_http_request_t *req,
                                  bb_http_host_capture_t *out)
{
    if (!req || !out) return BB_ERR_INVALID_ARG;
    if (s_cap.req != req) return BB_ERR_INVALID_ARG;

    out->status   = s_cap.status;
    out->body     = s_cap.body;
    out->body_len = s_cap.body_len;
    strncpy(out->content_type, s_cap.content_type,
            sizeof(out->content_type) - 1);
    out->content_type[sizeof(out->content_type) - 1] = '\0';

    // Disarm the slot (body ownership transferred to caller)
    s_cap.req  = NULL;
    s_cap.body = NULL;
    s_cap.body_len = 0;
    return BB_OK;
}

void bb_http_host_capture_free(bb_http_host_capture_t *cap)
{
    if (!cap) return;
    free(cap->body);
    cap->body     = NULL;
    cap->body_len = 0;
}

// ============================================================================
// STREAMING JSON ARRAY API — Host backend (buffered)
// ============================================================================

// Simple side-table for mapping request pointers to buffered arrays.
#define STREAM_TABLE_SIZE 8
static struct {
    bb_http_request_t *req;
    bb_json_t arr;
} s_stream_table[STREAM_TABLE_SIZE];

static bb_json_t stream_get_arr(bb_http_request_t *req) {
    for (int i = 0; i < STREAM_TABLE_SIZE; i++) {
        if (s_stream_table[i].req == req) return s_stream_table[i].arr;
    }
    return NULL;
}

static void stream_set_arr(bb_http_request_t *req, bb_json_t arr) {
    for (int i = 0; i < STREAM_TABLE_SIZE; i++) {
        if (s_stream_table[i].req == NULL) {
            s_stream_table[i].req = req;
            s_stream_table[i].arr = arr;
            return;
        }
    }
}

static void stream_clear_arr(bb_http_request_t *req) {
    for (int i = 0; i < STREAM_TABLE_SIZE; i++) {
        if (s_stream_table[i].req == req) {
            s_stream_table[i].req = NULL;
            s_stream_table[i].arr = NULL;
            return;
        }
    }
}

bb_err_t bb_http_resp_json_arr_begin(bb_http_request_t *req,
                                     bb_http_json_stream_t *out) {
    if (!req || !out) return BB_ERR_INVALID_ARG;

    bb_json_t arr = bb_json_arr_new();
    if (!arr) return BB_ERR_NO_SPACE;

    stream_set_arr(req, arr);

    out->_req = req;
    out->_err = BB_OK;
    out->_first = 1;
    out->_open = 1;

    return BB_OK;
}

bb_err_t bb_http_resp_json_arr_emit(bb_http_json_stream_t *stream,
                                    bb_json_t item) {
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;

    bb_http_request_t *req = (bb_http_request_t *)stream->_req;
    bb_json_t arr = stream_get_arr(req);
    if (!arr) return BB_ERR_INVALID_STATE;

    // Mark as no longer first
    stream->_first = 0;

    // Clone via serialize/parse (same pattern as Arduino backend)
    char *json_str = bb_json_item_serialize(item);
    if (!json_str) {
        // Serialize failed; append null
        bb_json_arr_append_string(arr, "null");
        return BB_OK;
    }

    bb_json_t cloned = bb_json_parse(json_str, 0);
    bb_json_free_str(json_str);
    if (!cloned) {
        // Parse failed; append null
        bb_json_arr_append_string(arr, "null");
        return BB_OK;
    }

    bb_json_arr_append_obj(arr, cloned);
    return BB_OK;
}

bb_err_t bb_http_resp_json_arr_end(bb_http_json_stream_t *stream) {
    if (!stream) return BB_ERR_INVALID_ARG;

    stream->_open = 0;
    bb_http_request_t *req = (bb_http_request_t *)stream->_req;
    bb_json_t arr = stream_get_arr(req);

    if (!arr) return BB_ERR_INVALID_STATE;

    // Send the buffered array
    bb_err_t err = bb_http_resp_send_json(req, arr);

    // Clean up
    bb_json_free(arr);
    stream_clear_arr(req);

    return stream->_err != BB_OK ? stream->_err : err;
}
