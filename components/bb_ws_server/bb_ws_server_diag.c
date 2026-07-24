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

#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_ws_server_diag_meta_rows[] = {
    { .key = "open_connections", .required = true },
};

const bb_serialize_desc_meta_t bb_ws_server_diag_meta = {
    .type_name = "websocket",
    .rows      = s_ws_server_diag_meta_rows,
    .n_rows    = sizeof(s_ws_server_diag_meta_rows) / sizeof(s_ws_server_diag_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// Describe-only route (B1-1180 PR-1 review fix) -- a PRODUCER-OWNED
// `static const` bb_route_t (handler=NULL), .rodata/flash, never DRAM
// (guarantee scoped to CONFIG_BB_OPENAPI_RUNTIME_META OFF, the default --
// see the `.responses` table just below). See bb_diag_section_t.
// describe_route's doc comment (components/bb_diag/include/
// bb_diag_section.h) for the full mechanism.
// ---------------------------------------------------------------------------

// CONFIG_BB_OPENAPI_RUNTIME_META (B1-1059 PR-3 batch 1) -- gated DIRECTLY on
// this Kconfig symbol, never on BB_SERIALIZE_META_SHIP: that macro also
// covers BB_SERIALIZE_META_HOST (unconditionally set by the plain `native`
// host env, see platformio.ini), which must NOT flip this section onto the
// runtime-compose path -- "meta tables compiled in for golden-testing"
// (SHIP) and "this route sources its schema at runtime" (RUNTIME_META) are
// deliberately distinct gates. Config OFF (default) is a zero-diff no-op:
// the `#else` arm below is byte-identical to the pre-migration table.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)

// Sized to the golden-tested hand literal (B1-1180 PR-1's
// test_bb_ws_server_diag_meta_golden.c proves the engine's output is
// byte-identical to BB_WS_SERVER_DIAG_SCHEMA_LITERAL) -- not an arbitrary
// round cap; any future desync trips bb_serialize_meta_openapi_schema()'s
// own bounded-buffer BB_ERR_NO_SPACE contract instead of silently
// truncating.
static char s_ws_server_diag_schema_buf[sizeof(BB_WS_SERVER_DIAG_SCHEMA_LITERAL)];

static bb_err_t assemble_schema(void)
{
    size_t n = 0;
    return bb_serialize_meta_openapi_schema(&bb_ws_server_diag_desc, &bb_ws_server_diag_meta,
                                             s_ws_server_diag_schema_buf,
                                             sizeof(s_ws_server_diag_schema_buf), &n);
}

// Mutable (`.data`, not `.rodata`) with this config on -- `.schema` starts
// NULL and is patched in once by bb_ws_server_diag_register() (or the test
// accessor below) before the route is ever registered/served.
static bb_route_response_t s_ws_server_diag_describe_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL /* patched at init */ },
    { .status = 0 },
};

#else

static const bb_route_response_t s_ws_server_diag_describe_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = BB_WS_SERVER_DIAG_SCHEMA_LITERAL },
    { .status = 0 },
};

#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

// Shared compose-and-patch step, called from both bb_ws_server_diag_
// register() and the test accessor below: idempotent (a second call is a
// pointer-stable no-op once `.schema` is set) and fail-loud (propagates
// assemble_schema()'s rc without ever patching a partial/NULL schema in).
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
static bb_err_t ensure_schema_patched(void)
{
    if (!s_ws_server_diag_describe_responses[0].schema) {
        bb_err_t rc = assemble_schema();
        if (rc != BB_OK) return rc;  // fail loud -- never register a NULL/partial schema
        s_ws_server_diag_describe_responses[0].schema = s_ws_server_diag_schema_buf;
    }
    return BB_OK;
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

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
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    bb_err_t patch_rc = ensure_schema_patched();
    if (patch_rc != BB_OK) return patch_rc;
#endif

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

// ---------------------------------------------------------------------------
// Test-only accessors (BB_WS_SERVER_DIAG_TESTING) -- see
// bb_ws_server_diag_test.h. Portable (not ESP_PLATFORM-gated): exercises
// the same guarded, idempotent compose-and-patch bb_ws_server_diag_
// register() runs, without requiring the ESP-IDF-gated register() itself.
// ---------------------------------------------------------------------------
#ifdef BB_WS_SERVER_DIAG_TESTING
#include "bb_ws_server_diag_test.h"

bb_err_t bb_ws_server_diag_assemble_schema_for_test(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return ensure_schema_patched();
#else
    return BB_OK;
#endif
}

const char *bb_ws_server_diag_get_describe_schema_for_test(void)
{
    return s_ws_server_diag_describe_responses[0].schema;
}
#endif /* BB_WS_SERVER_DIAG_TESTING */
