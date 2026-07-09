#pragma once

// bb_log_http — GET/POST /api/log/level routes sink, carved out of bb_log.
// No components/bb_log_http/include/ dir exists; this flat header is the
// codegen-visible surface (wire.py falls back to platform/espidf/<name>/*.h
// for components without a component-local include/).

#include "bb_core.h"
#include "bb_http_server.h"

// bbtool:init tier=regular fn=bb_log_register_routes_init server=true
bb_err_t bb_log_register_routes_init(bb_http_handle_t server);

// bbtool:init tier=pre_http fn=bb_log_register_routes_reserve
bb_err_t bb_log_register_routes_reserve(void);
