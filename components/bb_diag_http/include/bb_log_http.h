#pragma once

/**
 * @brief GET/POST /api/log/level routes, carved out of bb_log.
 */

// bb_log_http — GET/POST /api/log/level routes sink, carved out of bb_log
// (KB #708/#704).
//
// B1-1155 (KB 1477): the bb_log_http COMPONENT was dissolved -- these routes
// now live under bb_diag_http (an SSOT component owns no routes; this
// mirrors B1-1154's bb_storage_http dissolution). This header/its .c file
// moved verbatim, keeping their names -- bb_log itself still has zero
// dependency back on this header, preserving the original
// bb_http_server<->bb_log cycle-break this component existed to provide.
//
// CONFIG_BB_LOG_ROUTES (own Kconfig, default y) gates registration
// internally (see bb_log_http.c) so a consumer can compose bb_diag_http yet
// still opt out of the log-level route wiring at configure time.

#include "bb_core.h"
#include "bb_http_server.h"

// bbtool:init tier=regular fn=bb_log_register_routes_init server=true
bb_err_t bb_log_register_routes_init(bb_http_handle_t server);

// bbtool:init tier=pre_http fn=bb_log_register_routes_reserve
bb_err_t bb_log_register_routes_reserve(void);
