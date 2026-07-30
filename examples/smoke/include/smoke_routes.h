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
#endif

#ifdef __cplusplus
}
#endif
