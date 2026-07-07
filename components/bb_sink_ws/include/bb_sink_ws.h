#pragma once
// bb_sink_ws — bb_pub sink that forwards serialized telemetry payloads to
// subscribed WebSocket clients via bb_ws_server_broadcast_all.
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
 *   {"type":"push","topic":"<subtopic>","data":<original_json>}
 * and broadcasts the JSON text frame to WebSocket clients subscribed to that
 * topic (see the subscription-filtering rules in the implementation).
 *
 * Inbound frames are demuxed by envelope "type":
 *   {"type":"sub","topic":[...]}  -> replaces the client's subscription set.
 *   any other "type" (e.g. "cmd") -> RESERVED, ignored (no command path yet).
 *   legacy {"sub":[...]} (no "type") is still accepted for back-compat.
 *
 * `bb_sink_ws_init` registers the /ws WebSocket endpoint on `server` and wires
 * a real handler. Pass the already-started server handle.
 *
 * Returns BB_ERR_INVALID_ARG if out is NULL or endpoint registration fails.
 */
bb_err_t bb_sink_ws_init(bb_http_handle_t server, bb_pub_sink_t *out);

/**
 * Close all active /ws client sockets to reclaim heap during a TLS window.
 * Clients will auto-reconnect after the window closes. Idempotent.
 * No-op on host (no httpd sessions to close); sub tables are still cleared.
 */
bb_err_t bb_sink_ws_suspend(void);

/**
 * Re-enable /ws broadcast sends (clients reconnect on their own).
 * Exists for API symmetry with bb_sink_ws_suspend().
 */
void bb_sink_ws_resume(void);

#ifdef BB_SINK_WS_TESTING
/** Reset internal state for test isolation. */
void bb_sink_ws_reset_for_test(void);

/**
 * Host-only: inject a structured log event as if the "log" bb_event had fired.
 * Broadcasts {"ch":"log","data":<json>} to clients subscribed to "log".
 * Compiled only when BB_SINK_WS_TESTING is defined (native test builds).
 */
void bb_sink_ws_host_inject_log_event(const char *json);

/**
 * Host-only: override the malloc function used by the log-injection path.
 * Pass NULL to restore the default system malloc.
 * Used to exercise the alloc-failure/backoff path in tests.
 */
void bb_sink_ws_set_malloc(void *(*fn)(size_t));
#endif

#ifdef __cplusplus
}
#endif
