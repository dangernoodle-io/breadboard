// Host port for bb_http_client.
//
// Real network access is intentionally NOT implemented on host — host tests
// must be hermetic. Tests register a canned response via the test hook
// (bb_http_client_set_mock_response). If no mock is set, the call returns
// BB_ERR_INVALID_STATE so a missing mock surfaces immediately rather than
// silently hitting the wider internet.

#include "bb_http_client.h"
#include "bb_http_client_host.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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
    pthread_mutex_unlock(&s_mock_lock);
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
        out->status_code = 0;
        out->body_len = 0;
        out->truncated = false;
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

    out->status_code = m.status_code;
    out->body_len = copy;
    out->truncated = truncated;
    return BB_OK;
}
