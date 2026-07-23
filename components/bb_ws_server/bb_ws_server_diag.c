// bb_ws_server_diag -- see bb_ws_server_diag.h for the section contract.
// Pure/portable fill: bb_ws_server_open_count() (B1-1077 PR-3a).

#include "bb_ws_server_diag.h"

#include "bb_http.h"
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

// ---------------------------------------------------------------------------
// JSON Schema (B1-1180 PR-1) -- hand-authored, on-device (not host-gated;
// see the header's doc comment). Its byte-fidelity against the
// BB_SERIALIZE_META_HOST-gated co-located meta table below is proven by
// test/test_host/test_bb_ws_server_diag_meta_golden.c.
// ---------------------------------------------------------------------------

// A #define (not just the extern variable below) so the static-const
// describe route's response table (further down this file) can use the
// SAME literal text as a genuine compile-time constant expression --
// `.schema = bb_ws_server_diag_schema` (the VARIABLE's runtime value) is
// NOT a valid static/file-scope initializer in C ("initializer element is
// not constant"); `.schema = BB_WS_SERVER_DIAG_SCHEMA_LITERAL` (the
// macro-expanded string literal) is.
#define BB_WS_SERVER_DIAG_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"open_connections\":{\"type\":\"integer\"}}," \
    "\"required\":[\"open_connections\"]," \
    "\"additionalProperties\":false}"

const char *const bb_ws_server_diag_schema = BB_WS_SERVER_DIAG_SCHEMA_LITERAL;

#if defined(BB_SERIALIZE_META_HOST)

static const bb_serialize_field_meta_t s_ws_server_diag_meta_rows[] = {
    { .key = "open_connections", .required = true },
};

const bb_serialize_desc_meta_t bb_ws_server_diag_meta = {
    .type_name = "websocket",
    .rows      = s_ws_server_diag_meta_rows,
    .n_rows    = sizeof(s_ws_server_diag_meta_rows) / sizeof(s_ws_server_diag_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_HOST */

// ---------------------------------------------------------------------------
// Describe-only route (B1-1180 PR-1 review fix) -- a PRODUCER-OWNED
// `static const` bb_route_t (handler=NULL), .rodata/flash, never DRAM. See
// bb_diag_section_t.describe_route's doc comment
// (components/bb_diag/include/bb_diag_section.h) for the full mechanism.
// ---------------------------------------------------------------------------

static const bb_route_response_t s_ws_server_diag_describe_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = BB_WS_SERVER_DIAG_SCHEMA_LITERAL },
    { .status = 0 },
};

static const bb_route_t s_ws_server_diag_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/websocket",
    .tag       = "diag",
    .summary   = "current count of open WebSocket connections across all registered endpoints",
    .responses = s_ws_server_diag_describe_responses,
    .handler   = NULL,
};

#ifdef ESP_PLATFORM
bb_err_t bb_ws_server_diag_register(void)
{
    bb_diag_section_t section = {
        .name          = "websocket",
        .desc          = "current count of open WebSocket connections across all registered endpoints",
        .snap_desc     = &bb_ws_server_diag_desc,
        .fill          = bb_ws_server_diag_fill,
        .ctx           = NULL,
        .query_keys    = NULL,
        .n_query_keys  = 0,
        .describe_route = &s_ws_server_diag_describe_route,
    };
    return bb_diag_register_section(&section);
}
#endif
