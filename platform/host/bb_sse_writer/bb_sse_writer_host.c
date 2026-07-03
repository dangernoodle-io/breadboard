#include "bb_sse_writer.h"

// Host stub: bb_sse_writer_run is an ESP-IDF-only loop (FreeRTOS + sockets).
// This file exists so the component compiles cleanly on the host test target.
void bb_sse_writer_run(bb_http_request_t *req,
                       const char *connected_line,
                       bb_sse_wait_fn_t wait_fn,
                       bb_sse_cleanup_fn_t cleanup_fn,
                       bb_sse_done_fn_t done_fn,
                       void *ctx,
                       uint32_t wait_timeout_ms,
                       uint32_t heartbeat_ms)
{
    (void)req;
    (void)connected_line;
    (void)wait_fn;
    (void)cleanup_fn;
    (void)done_fn;
    (void)ctx;
    (void)wait_timeout_ms;
    (void)heartbeat_ms;
}
