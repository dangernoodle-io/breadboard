#include "bb_http_serialize_stream.h"

#include <stdbool.h>

// Bridges bb_serialize_json_stream_render()'s generic byte-sink flush_fn to
// bb_http_resp_send_chunk(). `failed` doubles as the `flush_failed` sticky
// abort flag bb_serialize_json polls after every flush -- once set, no
// further http_flush() calls occur (bb_json_put() short-circuits on the
// synthetic abort error).
typedef struct {
    bb_http_request_t *req;
    volatile bool       failed;
    bb_err_t            err;
} flush_ctx_t;

static void http_flush(void *vctx, const char *data, size_t len)
{
    flush_ctx_t *fc = vctx;
    if (fc->failed) return;
    bb_err_t err = bb_http_resp_send_chunk(fc->req, data, (int)len);
    if (err != BB_OK) {
        fc->failed = true;
        fc->err = err;
    }
}

bb_err_t bb_http_serialize_stream(bb_http_request_t *req,
                                   const bb_serialize_desc_t *desc, const void *snap)
{
    if (!req || !desc || !snap) return BB_ERR_INVALID_ARG;

    bb_err_t type_err = bb_http_resp_set_type(req, "application/json");
    if (type_err != BB_OK) return type_err;

    flush_ctx_t fc = { .req = req, .failed = false, .err = BB_OK };
    bb_err_t render_err = bb_serialize_json_stream_render(desc, snap, http_flush, &fc, &fc.failed);

    // Always finalize the chunked response, even on error -- an
    // unterminated chunked body can hang a strict client.
    bb_err_t fin_err = bb_http_resp_send_chunk(req, NULL, 0);

    if (fc.failed) return fc.err;
    if (render_err != BB_OK) return render_err;
    return fin_err;
}
