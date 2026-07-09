#pragma once

// bb_ring_diag — GET /api/diag/rings diagnostics route. No
// components/bb_ring_diag/include/ dir exists; this flat header is the
// codegen-visible surface (wire.py falls back to platform/espidf/<name>/*.h
// for components without a component-local include/).

#include "bb_core.h"
#include "bb_http_server.h"

// bbtool:init tier=regular fn=bb_ring_diag_init server=true
bb_err_t bb_ring_diag_init(bb_http_handle_t server);

// bbtool:init tier=pre_http fn=bb_ring_diag_reserve_routes
bb_err_t bb_ring_diag_reserve_routes(void);
