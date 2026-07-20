// bb_ws_server_diag -- see bb_ws_server_diag.h for the section contract.
// Pure/portable fill: bb_ws_server_open_count() (B1-1077 PR-3a).

#include "bb_ws_server_diag.h"

#include "bb_ws_server.h"

#include <stddef.h>

static const bb_serialize_field_t s_snap_fields[] = {
    { .key = "open_connections", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ws_server_diag_snap_t, open_connections) },
};

const bb_serialize_desc_t bb_ws_server_diag_desc = {
    .type_name = "websocket",
    .fields    = s_snap_fields,
    .n_fields  = sizeof(s_snap_fields) / sizeof(s_snap_fields[0]),
    .snap_size = sizeof(bb_ws_server_diag_snap_t),
};

bb_err_t bb_ws_server_diag_fill(void *dst, const bb_diag_fill_args_t *args)
{
    (void)args;
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_ws_server_diag_snap_t *snap = (bb_ws_server_diag_snap_t *)dst;
    snap->open_connections = (int64_t)bb_ws_server_open_count();
    return BB_OK;
}

#ifdef ESP_PLATFORM
bb_err_t bb_ws_server_diag_register(void)
{
    bb_diag_section_t section = {
        .name         = "websocket",
        .desc         = "current count of open WebSocket connections across all registered endpoints",
        .snap_desc    = &bb_ws_server_diag_desc,
        .fill         = bb_ws_server_diag_fill,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    return bb_diag_register_section(&section);
}
#endif
