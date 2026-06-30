// bb_http_body — portable request-body read helper (B1-335).
//
// Extracts the malloc→recv→NUL-terminate idiom shared by POST/PATCH/DELETE
// handlers. Compiled on both ESP-IDF (via CMakeLists SRCS) and host
// (via scripts/native_scaffold.py bb_http sources).

#include "bb_http_body.h"
#include "bb_http.h"
#include "bb_mem.h"

#include <stdlib.h>

// ---------------------------------------------------------------------------
// Allocator — swappable for OOM branch coverage in host tests
// ---------------------------------------------------------------------------

#ifdef BB_HTTP_BODY_TESTING
static void *(*s_malloc_fn)(size_t) = malloc;
void bb_http_body_set_malloc(void *(*fn)(size_t)) { s_malloc_fn = fn ? fn : malloc; }
#define BODY_MALLOC(sz) s_malloc_fn(sz)
#else
#define BODY_MALLOC(sz) bb_malloc_prefer_spiram(sz)
#endif

// ---------------------------------------------------------------------------
// Helper implementation
// ---------------------------------------------------------------------------

bb_err_t bb_http_req_recv_body_alloc(bb_http_request_t *req,
                                     size_t             max_bytes,
                                     char             **out_buf,
                                     int               *out_len)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0) {
        return BB_ERR_INVALID_ARG;
    }
    if ((size_t)body_len > max_bytes) {
        return BB_ERR_NO_SPACE;
    }

    char *buf = BODY_MALLOC((size_t)body_len + 1);
    if (!buf) {
        return BB_ERR_NO_SPACE;
    }

    int n = bb_http_req_recv(req, buf, (size_t)(body_len + 1));
    if (n < 0) {
        bb_mem_free(buf);
        return BB_ERR_INVALID_ARG;
    }

    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return BB_OK;
}
