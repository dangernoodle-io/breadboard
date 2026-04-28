#pragma once

#ifdef ESP_PLATFORM

#include "bb_nv.h"

/**
 * Register GET /api/logs (SSE stream) and GET /api/logs/status (JSON) on server.
 * Single-subscriber stream; external client (source=external) can preempt a
 * browser client, any other overlap returns 503.
 * `server` is a bb_http_handle_t (forward-declared as void* to keep
 * log_stream consumers free of the http_server dependency).
 */
bb_err_t bb_log_stream_register_routes(void *server);

/**
 * Register POST /api/log/level on server (bb_http_handle_t).
 * Body (URL-encoded): tag=<tag>&level=<error|warn|info|debug|verbose|none>
 * 204 on success, 400 on bad request.
 */
bb_err_t bb_log_register_routes(void *server);

#endif /* ESP_PLATFORM */
