// Host capture-stub send_fn backend for bb_data_http -- see
// bb_data_http_host.h. No pthread/threads: this is a plain synchronous
// capture into a fixed-size ring, called directly from
// bb_data_http_sweep_step()'s single-threaded drain phase.
#include "bb_data_http_host.h"
#include "bb_data_http.h"

// B1-1123 PR-1: send_fn now carries the client handle, not fd/is_ws --
// bb_data_http_client_t is component-private (bb_data_http_internal.h), not
// part of the public API, but platform backends (this file included) are
// allowed direct access to it, same precedent as
// platform/host/bb_led_gpio/bb_led_gpio.c's own relative include of its
// component's *_internal.h. This stub still captures fd/is_ws (tests assert
// on them) by reading them straight off the client handle.
#include "../../../components/bb_data_http/src/bb_data_http_internal.h"

#include "bb_str.h"

#include <string.h>

#define BB_DATA_HTTP_HOST_CAPTURE_MAX 32
#define BB_DATA_HTTP_HOST_FRAME_MAX   256

typedef struct {
    int    fd;
    bool   is_ws;
    char   key[BB_DATA_HTTP_KEY_MAX];
    size_t len;
    char   bytes[BB_DATA_HTTP_HOST_FRAME_MAX];
} capture_frame_t;

static capture_frame_t s_frames[BB_DATA_HTTP_HOST_CAPTURE_MAX];
static size_t          s_frame_count;

// B1-1424: fail-injection knob for the flush phase's retriable/fatal
// send-retry contract host tests -- see bb_data_http_host_fail_next()'s
// doc. s_fail_remaining == 0 means "no injected failure, capture normally".
static bb_err_t s_fail_rc;
static size_t   s_fail_remaining;

// B1-1424/B1-1429: last client passed to the installed abort_fn (see
// bb_data_http_host_install_abort()) -- host tests read this to assert a
// fatal send_fn failure actually reached the abort seam. NULL after
// bb_data_http_host_reset() or when no abort_fn call has happened yet.
static bb_data_http_client_t *s_last_aborted_client;

// B1-1123 PR-2: send_fn now also carries `key` -- captured here so host
// tests can assert the resolved key alongside the existing fd/is_ws/bytes
// fields (see bb_data_http_host_frame_key_at()).
static bb_err_t host_send(const char *key, const bb_data_http_client_t *client,
                          const void *bytes, size_t len, void *ctx)
{
    (void)ctx;
    if (s_fail_remaining > 0) {
        s_fail_remaining--;
        return s_fail_rc;
    }
    if (s_frame_count >= BB_DATA_HTTP_HOST_CAPTURE_MAX) return BB_ERR_NO_SPACE;

    capture_frame_t *f = &s_frames[s_frame_count];
    f->fd    = client ? client->fd : -1;  // LCOV_EXCL_BR_LINE -- host_send()'s only caller (bb_data_http_sweep_step()'s drain phase) always passes the live client `c` it is currently draining; never NULL.
    f->is_ws = client ? client->is_ws : false;  // LCOV_EXCL_BR_LINE -- same rationale as f->fd above.
    if (key) {  // LCOV_EXCL_BR_LINE -- resolve_outbound_key() (bb_data_http_common.c) never returns NULL; the seam's own contract (bb_data_http.h) forbids it.
        bb_strlcpy(f->key, key, sizeof(f->key));
    } else {
        // LCOV_EXCL_START
        f->key[0] = '\0';
        // LCOV_EXCL_STOP
    }
    f->len   = len > BB_DATA_HTTP_HOST_FRAME_MAX ? BB_DATA_HTTP_HOST_FRAME_MAX : len;
    if (f->len > 0 && bytes) memcpy(f->bytes, bytes, f->len);  // LCOV_EXCL_BR_LINE -- host_send()'s only caller (bb_data_http_sweep_step()'s drain phase) always passes a real stack buffer address; `bytes` can't actually be NULL when len > 0.

    s_frame_count++;
    return BB_OK;
}

void bb_data_http_host_install_send(void)
{
    bb_data_http_set_send_fn(host_send, NULL);
}

// B1-1424/B1-1429: records `client` into s_last_aborted_client -- host tests
// assert on bb_data_http_host_last_aborted_client() after injecting a fatal
// send_fn failure. Deliberately does NOT call bb_data_http_client_release()
// itself: that is the core's own no-abort-fn fallback (see
// bb_data_http_set_abort_fn()'s doc, bb_data_http.h) -- installing this stub
// exercises the "abort_fn IS installed" branch instead, so tests can tell
// the two apart.
static void host_abort(bb_data_http_client_t *client, void *ctx)
{
    (void)ctx;
    s_last_aborted_client = client;
    bb_data_http_client_release(client);
}

void bb_data_http_host_install_abort(void)
{
    bb_data_http_set_abort_fn(host_abort, NULL);
}

bb_data_http_client_t *bb_data_http_host_last_aborted_client(void)
{
    return s_last_aborted_client;
}

void bb_data_http_host_fail_next(bb_err_t rc, size_t count)
{
    s_fail_rc        = rc;
    s_fail_remaining = count;
}

void bb_data_http_host_reset(void)
{
    s_frame_count = 0;
    memset(s_frames, 0, sizeof(s_frames));
    s_fail_rc              = BB_OK;
    s_fail_remaining       = 0;
    s_last_aborted_client  = NULL;
}

size_t bb_data_http_host_frame_count(void)
{
    return s_frame_count;
}

// Shared bounds-check for both frame_at() and frame_key_at() below -- NULL
// on an out-of-range idx, otherwise the capture record.
static capture_frame_t *frame_at(size_t idx)
{
    if (idx >= s_frame_count) return NULL;
    return &s_frames[idx];
}

bb_err_t bb_data_http_host_frame_at(size_t idx, int *out_fd, bool *out_is_ws,
                                    char *buf, size_t buf_cap, size_t *out_len)
{
    capture_frame_t *f = frame_at(idx);
    if (!f) return BB_ERR_NOT_FOUND;

    if (out_fd) *out_fd = f->fd;
    if (out_is_ws) *out_is_ws = f->is_ws;
    if (out_len) *out_len = f->len;
    if (buf && buf_cap > 0) {
        size_t n = f->len < buf_cap ? f->len : buf_cap;
        memcpy(buf, f->bytes, n);
    }
    return BB_OK;
}

bb_err_t bb_data_http_host_frame_key_at(size_t idx, char *buf, size_t buf_cap)
{
    capture_frame_t *f = frame_at(idx);
    if (!f) return BB_ERR_NOT_FOUND;

    if (buf && buf_cap > 0) {
        bb_strlcpy(buf, f->key, buf_cap);
    }
    return BB_OK;
}
