#pragma once

// Private: gather-then-stream composition for the /api/health document
// (B1-1100, PR-5 of 6, epic B1-1054). Portable -- no ESP-IDF/FreeRTOS
// types, compiled for host + device, same as bb_health_wire.c/
// bb_health_emit.c. See bb_health_compose.c for the algorithm doc comment.
// Included by:
//   - platform/espidf/bb_health/bb_health.c (the ESP-IDF route handler,
//     which gathers the ROOT slice from bb_wifi/bb_mdns/bb_board and hands
//     it here)
//   - test/test_host/test_bb_health_compose.c

#include "bb_health_wire_priv.h"

#include "bb_http_serialize_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

// Composes and streams the full /api/health document to `req`: `root`
// (caller-pre-filled ok/validated/network) as a RAW group merging flat at
// the document root, followed by every currently-registered
// bb_health_section as a named OBJECT group -- e.g.
// {"ok":true,"validated":true,"network":{...},"mqtt":{...},"temp":{...}}.
//
// GATHER-THEN-STREAM: every registered section's fill hook runs into a
// request-scoped stack arena FIRST (phase 1). If ANY fill returns
// non-BB_OK, this function sends a clean HTTP 500 (via
// bb_http_send_json_error(), before any normal-body byte is written) and
// returns that result -- ZERO bytes of the normal document ever reach the
// wire. Only once every section's fill has succeeded does streaming begin
// (phase 2), via bb_http_serialize_stream_compose_ex() with
// f64_shortest=true (B1-1102 -- cJSON-identical shortest float rendering,
// e.g. temp's soc_c renders "55.3" not "55.300000"). Once phase 2 starts it
// cannot structurally fail -- see bb_http_serialize_stream_compose_ex()'s
// own doc comment for the streaming-abort tradeoff on a genuine mid-stream
// I/O error.
//
// Returns BB_ERR_INVALID_ARG if `req` or `root` is NULL.
bb_err_t bb_health_compose_and_stream(bb_http_request_t *req, const bb_health_wire_t *root);

#ifdef __cplusplus
}
#endif
