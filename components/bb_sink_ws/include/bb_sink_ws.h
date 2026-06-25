#pragma once
// bb_sink_ws — bb_pub sink that forwards serialized telemetry payloads to
// subscribed WebSocket clients via bb_websocket_broadcast_all.
//
// Usage:
//   bb_http_handle_t server = ...; // already-started HTTP server handle
//   bb_pub_sink_t s;
//   bb_sink_ws_init(server, &s);
//   bb_pub_add_sink(&s);

#include "bb_core.h"
#include "bb_pub.h"
#include "bb_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the /ws WebSocket endpoint on `server` and fill `out` with a
 * bb_pub_sink_t that, on each publish, wraps the payload as
 *   {"ch":"<subtopic>","data":<original_json>}
 * and broadcasts the JSON text frame to all currently-active WebSocket clients
 * on `server` via bb_websocket_broadcast_all.
 *
 * `bb_sink_ws_init` registers the /ws WebSocket endpoint on `server` and wires
 * a real handler. Pass the already-started server handle.
 *
 * Returns BB_ERR_INVALID_ARG if out is NULL or endpoint registration fails.
 */
bb_err_t bb_sink_ws_init(bb_http_handle_t server, bb_pub_sink_t *out);

#ifdef BB_SINK_WS_TESTING
/** Reset internal state for test isolation. */
void bb_sink_ws_reset_for_test(void);

/**
 * Host-only: inject a log line as if the logs pump had drained it.
 * Broadcasts {"ch":"logs","data":"<line>"} to clients subscribed to "logs".
 * Compiled only when BB_SINK_WS_TESTING is defined (native test builds).
 */
bb_err_t bb_sink_ws_host_inject_log_line(const char *line);
#endif

#ifdef __cplusplus
}
#endif
