// Host port for bb_http_client.
//
// Real network access is intentionally NOT implemented on host — host tests
// must be hermetic. Tests register a canned response via the test hook
// (bb_http_client_set_mock_response). If no mock is set, the call returns
// BB_ERR_INVALID_STATE so a missing mock surfaces immediately rather than
// silently hitting the wider internet.

#include "bb_http_client.h"
#include "bb_http_client_host.h"
#include "bb_str.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const char *body;       // borrowed; tests typically pass a string literal
    size_t      body_len;
    int         status_code;
    bb_err_t    transport_result;  // BB_OK = mock present, else returned as-is
} mock_state_t;

static mock_state_t s_mock = {
    .body = NULL,
    .body_len = 0,
    .status_code = 0,
    .transport_result = BB_ERR_INVALID_STATE,  // no mock => fail loudly
};
static pthread_mutex_t s_mock_lock = PTHREAD_MUTEX_INITIALIZER;

static bb_http_client_post_record_t s_last_post = { .called = false };

// ---------------------------------------------------------------------------
// Session mock state (declared early so clear_mock can reference them)
// ---------------------------------------------------------------------------

typedef struct {
    int      canned_status;
    bb_err_t transport_result;
    int      tls_error_code;
} session_mock_state_t;

static session_mock_state_t s_session_mock = {
    .canned_status    = 200,
    .transport_result = BB_OK,
};
static bb_http_client_session_record_t s_session_last = { .called = false };

// Header capture state (declared here so clear_mock can reference them).
static bb_http_client_header_record_t s_headers[BB_HTTP_CLIENT_HOST_MAX_HEADERS];
static int s_header_count = 0;

// Session open tracking.
static int  s_session_open_count    = 0;
static bool s_session_last_keep_alive = false;

void bb_http_client_set_mock_response(const char *body, size_t body_len,
                                      int status_code)
{
    pthread_mutex_lock(&s_mock_lock);
    s_mock.body = body;
    s_mock.body_len = body_len;
    s_mock.status_code = status_code;
    s_mock.transport_result = BB_OK;
    pthread_mutex_unlock(&s_mock_lock);
}

void bb_http_client_set_mock_transport_error(bb_err_t err)
{
    pthread_mutex_lock(&s_mock_lock);
    s_mock.body = NULL;
    s_mock.body_len = 0;
    s_mock.status_code = 0;
    s_mock.transport_result = err;
    pthread_mutex_unlock(&s_mock_lock);
}

void bb_http_client_clear_mock(void)
{
    pthread_mutex_lock(&s_mock_lock);
    s_mock.body = NULL;
    s_mock.body_len = 0;
    s_mock.status_code = 0;
    s_mock.transport_result = BB_ERR_INVALID_STATE;
    s_last_post = (bb_http_client_post_record_t){ .called = false };
    s_session_mock.canned_status    = 200;
    s_session_mock.transport_result = BB_OK;
    s_session_mock.tls_error_code   = 0;
    s_session_last = (bb_http_client_session_record_t){ .called = false };
    memset(s_headers, 0, sizeof(s_headers));
    s_header_count = 0;
    s_session_open_count = 0;
    s_session_last_keep_alive = false;
    pthread_mutex_unlock(&s_mock_lock);
}

void bb_http_client_session_set_mock_status(int status_code)
{
    pthread_mutex_lock(&s_mock_lock);
    s_session_mock.canned_status    = status_code;
    s_session_mock.transport_result = BB_OK;
    pthread_mutex_unlock(&s_mock_lock);
}

void bb_http_client_session_set_mock_tls_error_code(int code)
{
    pthread_mutex_lock(&s_mock_lock);
    s_session_mock.tls_error_code = code;
    pthread_mutex_unlock(&s_mock_lock);
}

void bb_http_client_session_set_mock_transport_error(bb_err_t err)
{
    pthread_mutex_lock(&s_mock_lock);
    s_session_mock.transport_result = err;
    pthread_mutex_unlock(&s_mock_lock);
}

bb_http_client_session_record_t bb_http_client_session_last_post(void)
{
    pthread_mutex_lock(&s_mock_lock);
    bb_http_client_session_record_t rec = s_session_last;
    pthread_mutex_unlock(&s_mock_lock);
    return rec;
}

// ---------------------------------------------------------------------------
// Header capture accessors
// ---------------------------------------------------------------------------

int bb_http_client_session_header_count(void)
{
    pthread_mutex_lock(&s_mock_lock);
    int n = s_header_count;
    pthread_mutex_unlock(&s_mock_lock);
    return n;
}

bb_http_client_header_record_t bb_http_client_session_header_at(int i)
{
    pthread_mutex_lock(&s_mock_lock);
    bb_http_client_header_record_t rec;
    memset(&rec, 0, sizeof(rec));
    if (i >= 0 && i < s_header_count) {
        rec = s_headers[i];
    }
    pthread_mutex_unlock(&s_mock_lock);
    return rec;
}

bb_http_client_header_record_t bb_http_client_session_find_header(const char *name)
{
    pthread_mutex_lock(&s_mock_lock);
    bb_http_client_header_record_t rec;
    memset(&rec, 0, sizeof(rec));
    if (name) {
        for (int i = 0; i < s_header_count; i++) {
            if (strcmp(s_headers[i].name, name) == 0) {
                rec = s_headers[i];
                break;
            }
        }
    }
    pthread_mutex_unlock(&s_mock_lock);
    return rec;
}

// Session handle — opaque struct; on host just holds the url_base string.
typedef struct {
    char url_base[256];
} host_session_t;

int bb_http_client_session_open_count(void)
{
    pthread_mutex_lock(&s_mock_lock);
    int n = s_session_open_count;
    pthread_mutex_unlock(&s_mock_lock);
    return n;
}

bool bb_http_client_session_last_keep_alive(void)
{
    pthread_mutex_lock(&s_mock_lock);
    bool v = s_session_last_keep_alive;
    pthread_mutex_unlock(&s_mock_lock);
    return v;
}

bb_err_t bb_http_client_session_open(const bb_http_client_cfg_t *cfg,
                                     const char *url_base,
                                     bb_http_client_session_t *out)
{
    if (!url_base || !out) return BB_ERR_INVALID_ARG;
    host_session_t *s = (host_session_t *)calloc(1, sizeof(host_session_t));
    if (!s) return BB_ERR_NO_SPACE;
    bb_strlcpy(s->url_base, url_base, sizeof(s->url_base));
    // Reset header capture for the new session; record open.
    pthread_mutex_lock(&s_mock_lock);
    memset(s_headers, 0, sizeof(s_headers));
    s_header_count = 0;
    s_session_open_count++;
    s_session_last_keep_alive = (cfg && cfg->keep_alive);
    pthread_mutex_unlock(&s_mock_lock);
    *out = (bb_http_client_session_t)s;
    return BB_OK;
}

bb_err_t bb_http_client_session_set_header(bb_http_client_session_t s,
                                            const char *name,
                                            const char *value)
{
    if (!s || !name || !value) return BB_ERR_INVALID_ARG;

    pthread_mutex_lock(&s_mock_lock);
    // Update existing entry if name already present.
    for (int i = 0; i < s_header_count; i++) {
        if (strcmp(s_headers[i].name, name) == 0) {
            strncpy(s_headers[i].value, value, BB_HTTP_CLIENT_HOST_HEADER_VALUE_MAX - 1);
            s_headers[i].value[BB_HTTP_CLIENT_HOST_HEADER_VALUE_MAX - 1] = '\0';
            pthread_mutex_unlock(&s_mock_lock);
            return BB_OK;
        }
    }
    // Append new entry.
    if (s_header_count < BB_HTTP_CLIENT_HOST_MAX_HEADERS) {
        strncpy(s_headers[s_header_count].name,  name,  BB_HTTP_CLIENT_HOST_HEADER_NAME_MAX  - 1);
        strncpy(s_headers[s_header_count].value, value, BB_HTTP_CLIENT_HOST_HEADER_VALUE_MAX - 1);
        s_headers[s_header_count].name[BB_HTTP_CLIENT_HOST_HEADER_NAME_MAX   - 1] = '\0';
        s_headers[s_header_count].value[BB_HTTP_CLIENT_HOST_HEADER_VALUE_MAX - 1] = '\0';
        s_header_count++;
    }
    pthread_mutex_unlock(&s_mock_lock);
    return BB_OK;
}

bb_err_t bb_http_client_session_post(bb_http_client_session_t s,
                                     const char *url,
                                     const char *body, size_t body_len,
                                     const char *content_type,
                                     bb_http_client_result_t *out)
{
    if (!s || !url || !out) return BB_ERR_INVALID_ARG;

    pthread_mutex_lock(&s_mock_lock);
    session_mock_state_t m = s_session_mock;

    s_session_last.called       = true;
    strncpy(s_session_last.url, url, BB_HTTP_CLIENT_SESSION_URL_MAX - 1);
    s_session_last.url[BB_HTTP_CLIENT_SESSION_URL_MAX - 1] = '\0';
    s_session_last.body         = body;
    s_session_last.body_len     = body_len;
    s_session_last.content_type = content_type ? content_type : "application/json";
    s_session_last.canned_status = m.canned_status;
    pthread_mutex_unlock(&s_mock_lock);

    if (m.transport_result != BB_OK) {
        out->status_code  = 0;
        out->body_len     = 0;
        out->truncated    = false;
        out->tls_error_code = 0;
        return m.transport_result;
    }

    out->status_code  = m.canned_status;
    out->body_len     = 0;
    out->truncated    = false;
    out->tls_error_code = m.tls_error_code;
    return BB_OK;
}

void bb_http_client_session_close(bb_http_client_session_t s)
{
    if (!s) return;
    free(s);
}

bb_http_client_post_record_t bb_http_client_get_last_post(void)
{
    pthread_mutex_lock(&s_mock_lock);
    bb_http_client_post_record_t rec = s_last_post;
    pthread_mutex_unlock(&s_mock_lock);
    return rec;
}

bb_err_t bb_http_client_get(const char *url,
                            char *body, size_t body_cap,
                            const bb_http_client_cfg_t *cfg,
                            bb_http_client_result_t *out)
{
    (void)cfg;
    if (!url || !body || body_cap == 0 || !out) return BB_ERR_INVALID_ARG;

    pthread_mutex_lock(&s_mock_lock);
    mock_state_t m = s_mock;
    pthread_mutex_unlock(&s_mock_lock);

    if (m.transport_result != BB_OK) {
        out->status_code    = 0;
        out->body_len       = 0;
        out->truncated      = false;
        out->tls_error_code = 0;
        return m.transport_result;
    }

    size_t copy = m.body_len;
    bool truncated = false;
    if (copy >= body_cap) {
        copy = body_cap - 1;
        truncated = true;
    }
    if (copy > 0 && m.body) {
        memcpy(body, m.body, copy);
    }
    body[copy] = '\0';

    out->status_code    = m.status_code;
    out->body_len       = copy;
    out->truncated      = truncated;
    out->tls_error_code = 0;
    return BB_OK;
}

bb_err_t bb_http_client_get_stream(const char *url,
                                   bb_http_client_chunk_cb cb, void *ctx,
                                   const bb_http_client_cfg_t *cfg,
                                   bb_http_client_result_t *out)
{
    (void)cfg;
    if (!url || !cb || !out) return BB_ERR_INVALID_ARG;

    pthread_mutex_lock(&s_mock_lock);
    mock_state_t m = s_mock;
    pthread_mutex_unlock(&s_mock_lock);

    if (m.transport_result != BB_OK) {
        out->status_code    = 0;
        out->body_len       = 0;
        out->truncated      = false;
        out->tls_error_code = 0;
        return m.transport_result;
    }

    // Replay the mock body in ~256-byte chunks so streaming tests exercise
    // chunk-boundary handling. Chunk size must be > 0 even for tiny bodies.
    const size_t chunk_size = 256;
    size_t total = 0;
    size_t remaining = m.body_len;
    bb_err_t cb_err = BB_OK;

    while (remaining > 0 && cb_err == BB_OK) {
        size_t n = remaining < chunk_size ? remaining : chunk_size;
        cb_err = cb(ctx, m.body ? m.body + total : "", n);
        total     += n;
        remaining -= n;
    }

    out->status_code    = m.status_code;
    out->body_len       = total;
    out->truncated      = (cb_err == BB_ERR_NO_SPACE);
    out->tls_error_code = 0;
    if (cb_err != BB_OK) return cb_err;
    return BB_OK;
}

bb_err_t bb_http_client_post(const char *url,
                             const char *body, size_t body_len,
                             const char *content_type,
                             char *resp, size_t resp_cap,
                             const bb_http_client_cfg_t *cfg,
                             bb_http_client_result_t *out)
{
    if (!url || !resp || resp_cap == 0 || !out) return BB_ERR_INVALID_ARG;

    pthread_mutex_lock(&s_mock_lock);
    mock_state_t m = s_mock;

    // Record the POST for test inspection.
    s_last_post.called          = true;
    s_last_post.method          = "POST";
    s_last_post.url             = url;
    s_last_post.body            = body;
    s_last_post.body_len        = body_len;
    s_last_post.content_type    = content_type ? content_type : "application/json";
    s_last_post.has_client_cert = (cfg && cfg->client_cert_pem != NULL);
    s_last_post.has_client_key  = (cfg && cfg->client_key_pem  != NULL);
    s_last_post.has_ca_cert     = (cfg && cfg->ca_cert_pem     != NULL);
    pthread_mutex_unlock(&s_mock_lock);

    if (m.transport_result != BB_OK) {
        out->status_code    = 0;
        out->body_len       = 0;
        out->truncated      = false;
        out->tls_error_code = 0;
        return m.transport_result;
    }

    size_t copy = m.body_len;
    bool truncated = false;
    if (copy >= resp_cap) {
        copy = resp_cap - 1;
        truncated = true;
    }
    if (copy > 0 && m.body) {
        memcpy(resp, m.body, copy);
    }
    resp[copy] = '\0';

    out->status_code    = m.status_code;
    out->body_len       = copy;
    out->truncated      = truncated;
    out->tls_error_code = 0;
    return BB_OK;
}
