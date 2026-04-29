#pragma once

#include "bb_nv.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#include "bb_json.h"

typedef void (*bb_info_extender_fn)(bb_json_t root);
#else
// Host stub: opaque handle for extender callbacks
typedef void (*bb_info_extender_fn)(void *root);
#endif

// Register an extender. Fixed capacity (4 slots). Must be called before
// bb_http_server_start; registering after start returns BB_ERR_INVALID_ARG.
// Returns BB_ERR_NO_SPACE if the table is full.
bb_err_t bb_info_register_extender(bb_info_extender_fn fn);

// Register an extender for /api/health. Same fn signature as
// bb_info_register_extender; called separately so /api/info and
// /api/health get distinct extension hooks. Fixed capacity (4 slots).
// Must be called before bb_http_server_start.
bb_err_t bb_health_register_extender(bb_info_extender_fn fn);

#ifdef __cplusplus
}
#endif
