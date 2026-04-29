// Host stubs for bb_http functions that require a real HTTP server backend.
// These are no-ops used only by the native (host) test environment.
// The route registry (route_registry.c) is portable and compiled directly.
#include "bb_http.h"

// Test hook: force bb_http_register_route to fail
static bool s_force_register_fail = false;

void bb_http_host_force_register_fail(bool fail)
{
    s_force_register_fail = fail;
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
    (void)req;
    (void)status_code;
    return BB_OK;
}

bb_err_t bb_http_resp_set_type(bb_http_request_t *req, const char *mime)
{
    (void)req;
    (void)mime;
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
    (void)req;
    (void)body;
    (void)len;
    return BB_OK;
}

bb_err_t bb_http_resp_send_err(bb_http_request_t *req, int status_code, const char *message)
{
    (void)req;
    (void)status_code;
    (void)message;
    return BB_OK;
}

int bb_http_req_body_len(bb_http_request_t *req)
{
    (void)req;
    return 0;
}

int bb_http_req_recv(bb_http_request_t *req, char *buf, size_t buf_size)
{
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
    (void)req;
    (void)str;
    return BB_OK;
}

bb_err_t bb_http_resp_send_chunk(bb_http_request_t *req, const char *buf, int len)
{
    (void)req;
    (void)buf;
    (void)len;
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
