#pragma once

#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// SSE writer callback types
// ---------------------------------------------------------------------------

// wait_fn: called each loop iteration to produce a single SSE frame.
//
// Return contract:
//   > 0  : frame bytes written to buf (data ready, send the frame)
//   = 0  : idle timeout (no data this interval)
//   < 0  : hard error / stop (terminate loop)
typedef int (*bb_sse_wait_fn_t)(void *ctx, char *buf, size_t buflen,
                                uint32_t timeout_ms);

// cleanup_fn: called after the loop exits, before async_handler_complete.
// May be NULL. Responsible for releasing ctx resources.
typedef void (*bb_sse_cleanup_fn_t)(void *ctx);

// ---------------------------------------------------------------------------
// SSE writer main loop
// ---------------------------------------------------------------------------
//
// Runs the full SSE server-push loop for one client connection:
//   - Sets SO_RCVTIMEO (30 s) and TCP_NODELAY on the socket fd
//   - Sends standard SSE response headers
//   - Sends connected_line as the first SSE comment/event
//   - Loops: peer-FIN probe, wait_fn call, frame send, heartbeat ping
//   - Calls cleanup_fn(ctx) when the loop exits (if non-NULL)
//   - Calls bb_http_req_async_handler_complete(req)
//   - Calls vTaskDelete(NULL) — does NOT return
//
// Parameters:
//   req              - async HTTP request handle
//   connected_line   - first SSE comment/event (e.g. ": connected\n\n")
//   wait_fn          - callback to produce one SSE frame per iteration
//   cleanup_fn       - optional cleanup called after loop exits (may be NULL)
//   ctx              - passed to wait_fn and cleanup_fn
//   wait_timeout_ms  - per-call timeout passed to wait_fn
//   heartbeat_ms     - accumulated idle threshold before sending ": ping\n\n"
void bb_sse_writer_run(bb_http_request_t *req,
                       const char *connected_line,
                       bb_sse_wait_fn_t wait_fn,
                       bb_sse_cleanup_fn_t cleanup_fn,
                       void *ctx,
                       uint32_t wait_timeout_ms,
                       uint32_t heartbeat_ms);

#ifdef __cplusplus
}
#endif
