// bb_sensors — per-section /api/sensors/* dispatch (fan/power/thermal).
//
// FULL BREAK (B1-828 PR-2): the old composite GET/PATCH /api/sensors
// endpoint (bb_response-backed) is gone -- no back-compat shim, no dual
// shape. Each section is now its own bb_data binding served at
// /api/sensors/<key> via the shared bb_http_section dispatch helper
// (components/bb_http_server):
//
//   GET/PATCH /api/sensors/fan     — CONFIG resource (autofan cfg, or
//                                     manual duty_pct when autofan is not
//                                     compiled in). NOT fan telemetry --
//                                     rpm/die_c/board_c are not served here.
//   GET       /api/sensors/power   — read-only telemetry; PATCH -> 405.
//   GET       /api/sensors/thermal — read-only telemetry; PATCH -> 405.
//
// There is no bespoke section-registration API anymore: any consumer that
// calls bb_data_bind() with its own key (before bb_sensors_init() runs) is
// served automatically at /api/sensors/<key> -- the render/apply adapters
// (bb_sensors_dispatch.c) are generic over bb_data.
//
// Host twin: none. bb_sensors_init() itself is ESP-IDF only (it drives
// bb_http_section_init(), which registers real httpd routes); the portable
// bind step (bb_sensors_bind_and_register(), bb_sensors_dispatch_priv.h) is
// what host tests drive directly.
#pragma once
#include "bb_core.h"
#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Binds the built-in fan/power/thermal bb_data keys and registers GET+PATCH
// /api/sensors/* with the HTTP server via the bb_http_section dispatch
// helper (regular-tier init fn).
// bbtool:init tier=regular fn=bb_sensors_init server=true
bb_err_t bb_sensors_init(bb_http_handle_t server);

#ifdef __cplusplus
}
#endif
