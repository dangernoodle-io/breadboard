// Host capture-stub send_fn backend for bb_data_http -- see
// bb_data_http_host.h. No pthread/threads: this is a plain synchronous
// capture into a fixed-size ring, called directly from
// bb_data_http_sweep_step()'s single-threaded drain phase.
#include "bb_data_http_host.h"
#include "bb_data_http.h"

#include <string.h>

#define BB_DATA_HTTP_HOST_CAPTURE_MAX 32
#define BB_DATA_HTTP_HOST_FRAME_MAX   256

typedef struct {
    int    fd;
    bool   is_ws;
    size_t len;
    char   bytes[BB_DATA_HTTP_HOST_FRAME_MAX];
} capture_frame_t;

static capture_frame_t s_frames[BB_DATA_HTTP_HOST_CAPTURE_MAX];
static size_t          s_frame_count;

static bb_err_t host_send(int fd, bool is_ws, const void *bytes, size_t len, void *ctx)
{
    (void)ctx;
    if (s_frame_count >= BB_DATA_HTTP_HOST_CAPTURE_MAX) return BB_ERR_NO_SPACE;

    capture_frame_t *f = &s_frames[s_frame_count];
    f->fd    = fd;
    f->is_ws = is_ws;
    f->len   = len > BB_DATA_HTTP_HOST_FRAME_MAX ? BB_DATA_HTTP_HOST_FRAME_MAX : len;
    if (f->len > 0 && bytes) memcpy(f->bytes, bytes, f->len);  // LCOV_EXCL_BR_LINE -- host_send()'s only caller (bb_data_http_sweep_step()'s drain phase) always passes a real stack buffer address; `bytes` can't actually be NULL when len > 0.

    s_frame_count++;
    return BB_OK;
}

void bb_data_http_host_install_send(void)
{
    bb_data_http_set_send_fn(host_send, NULL);
}

void bb_data_http_host_reset(void)
{
    s_frame_count = 0;
    memset(s_frames, 0, sizeof(s_frames));
}

size_t bb_data_http_host_frame_count(void)
{
    return s_frame_count;
}

bb_err_t bb_data_http_host_frame_at(size_t idx, int *out_fd, bool *out_is_ws,
                                    char *buf, size_t buf_cap, size_t *out_len)
{
    if (idx >= s_frame_count) return BB_ERR_NOT_FOUND;

    capture_frame_t *f = &s_frames[idx];
    if (out_fd) *out_fd = f->fd;
    if (out_is_ws) *out_is_ws = f->is_ws;
    if (out_len) *out_len = f->len;
    if (buf && buf_cap > 0) {
        size_t n = f->len < buf_cap ? f->len : buf_cap;
        memcpy(buf, f->bytes, n);
    }
    return BB_OK;
}
