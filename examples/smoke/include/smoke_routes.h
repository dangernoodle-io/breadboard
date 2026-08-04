#pragma once

// Route handlers + descriptors defined in smoke_app.c, declared here so
// examples/smoke/main/bb_wire.h (the `bbtool codegen` consumer manifest --
// see scripts/bbtool/README.md's "`args=` (parameterized init calls) and
// `--consumer-manifest`" section) can name them in a generated call. The
// generated composition root (main/generated/bb_app_init.c) is a SEPARATE
// translation unit, so these can no longer be `static` in smoke_app.c --
// composition-only (CLAUDE.md): examples/smoke is codegen-composed, and a
// hand-wired registration call here would be a hole in what smoke exists
// to prove.

#include "bb_http.h"
#ifdef ESP_PLATFORM
#include "bb_ws_server.h"
#include "bb_data.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

bb_err_t smoke_ping_handler(bb_http_request_t *req);

#ifdef ESP_PLATFORM
// WebSocket echo handler + /api/wsbcast broadcast handler (bb_ws_server
// demo) -- ESP-IDF only, mirroring smoke_app.c's own ESP_PLATFORM guard.
bb_err_t smoke_ws_echo_handler(bb_http_request_t *req,
                               const bb_ws_server_frame_t *frame);
bb_err_t smoke_wsbcast_handler(bb_http_request_t *req);

extern const bb_route_t smoke_ws_route;

// B1-1425: "log" bb_data key producer wiring -- the bb_data_gather_plain()
// thunk's typed fill pointer, wrapping bb_log_event_gather() (components/
// bb_log_event/include/bb_log_event_wire.h, ESP-IDF only, same shape as
// examples/floor/main/floor_app.c's own s_log_fill_ctx). Not static -- see
// smoke_ping_handler's comment above; examples/smoke/main/bb_wire.h's
// bb_data_bind() manifest entry references this by address so the "log" key
// bb_data_http_espidf_routes_init()/_ws_routes_init() serve at GET
// /api/events?topic=log and GET /ws/events actually carries real traffic
// instead of streaming nothing.
extern const bb_data_plain_fill_ctx_t smoke_log_fill_ctx;
#endif

#ifdef __cplusplus
}
#endif
