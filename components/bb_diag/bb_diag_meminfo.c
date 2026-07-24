// bb_diag_meminfo -- see bb_diag_meminfo.h for the section contract. Thin
// adapter: bb_diag_fill_fn's signature is untyped (void *dst, const
// bb_diag_fill_args_t *args), so this shim is the only cast needed over
// bb_meminfo_heap_snap_fill()'s typed signature.

#include "bb_diag_meminfo.h"

#include "bb_http.h"
#include "bb_meminfo_heap_snap.h"

bb_err_t bb_diag_meminfo_fill(void *dst, const bb_diag_fill_args_t *args)
{
    (void)args;
    return bb_meminfo_heap_snap_fill((bb_meminfo_heap_snap_t *)dst);
}

// ---------------------------------------------------------------------------
// Describe-only route (B1-1180 PR-1 review fix) -- a PRODUCER-OWNED
// `static const` bb_route_t (handler=NULL), .rodata/flash, never DRAM
// (guarantee scoped to CONFIG_BB_OPENAPI_RUNTIME_META OFF, the default --
// see the `.responses` table just below). See bb_diag_section_t.
// describe_route's doc comment (components/bb_diag/include/
// bb_diag_section.h) for the full mechanism. Lives here (the section's
// registrar) rather than in bb_meminfo_heap_snap.c -- that file's own
// header (bb_meminfo_heap_snap.h) is a genuinely public bb_meminfo surface
// and must not gain a bb_route_t dependency; the schema literal/meta table
// it already exports (bb_meminfo_heap_snap_schema/_meta) is enough for
// this route.
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
// test_bb_meminfo_heap_snap_meta_golden.c proves the engine's output is
// byte-identical to BB_MEMINFO_HEAP_SNAP_SCHEMA_LITERAL) -- not an
// arbitrary round cap; any future desync trips bb_serialize_meta_openapi_
// schema()'s own bounded-buffer BB_ERR_NO_SPACE contract instead of
// silently truncating.
static char s_diag_meminfo_schema_buf[sizeof(BB_MEMINFO_HEAP_SNAP_SCHEMA_LITERAL)];

static bb_err_t assemble_schema(void)
{
    size_t n = 0;
    return bb_serialize_meta_openapi_schema(&bb_meminfo_heap_snap_desc, &bb_meminfo_heap_snap_meta,
                                             s_diag_meminfo_schema_buf,
                                             sizeof(s_diag_meminfo_schema_buf), &n);
}

// Mutable (`.data`, not `.rodata`) with this config on -- `.schema` starts
// NULL and is patched in once by bb_diag_meminfo_register() (or the test
// accessor below) before the route is ever registered/served.
static bb_route_response_t s_diag_meminfo_describe_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL /* patched at init */ },
    { .status = 0 },
};

#else

static const bb_route_response_t s_diag_meminfo_describe_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = BB_MEMINFO_HEAP_SNAP_SCHEMA_LITERAL },
    { .status = 0 },
};

#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

// Shared compose-and-patch step, called from both bb_diag_meminfo_
// register() and the test accessor below: idempotent (a second call is a
// pointer-stable no-op once `.schema` is set) and fail-loud (propagates
// assemble_schema()'s rc without ever patching a partial/NULL schema in).
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
static bb_err_t ensure_schema_patched(void)
{
    if (!s_diag_meminfo_describe_responses[0].schema) {
        bb_err_t rc = assemble_schema();
        if (rc != BB_OK) return rc;  // fail loud -- never register a NULL/partial schema
        s_diag_meminfo_describe_responses[0].schema = s_diag_meminfo_schema_buf;
    }
    return BB_OK;
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

static const bb_route_t s_diag_meminfo_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/meminfo",
    .tag       = "diag",
    .summary   = "heap memory snapshot",
    .responses = s_diag_meminfo_describe_responses,
    .handler   = NULL,
};

#ifdef ESP_PLATFORM
bb_err_t bb_diag_meminfo_register(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    bb_err_t patch_rc = ensure_schema_patched();
    if (patch_rc != BB_OK) return patch_rc;
#endif

    bb_diag_section_t section = {
        .name           = "meminfo",
        .desc           = "heap memory snapshot",
        .snap_desc      = &bb_meminfo_heap_snap_desc,
        .fill           = bb_diag_meminfo_fill,
        .ctx            = NULL,
        .query_keys     = NULL,
        .n_query_keys   = 0,
        .describe_route = &s_diag_meminfo_describe_route,
    };
    return bb_diag_register_section(&section);
}
#endif

// ---------------------------------------------------------------------------
// Test-only accessors (BB_DIAG_MEMINFO_TESTING) -- see
// bb_diag_meminfo_test.h. Portable (not ESP_PLATFORM-gated): exercises the
// same guarded, idempotent compose-and-patch bb_diag_meminfo_register()
// runs, without requiring the ESP-IDF-gated register() itself.
// ---------------------------------------------------------------------------
#ifdef BB_DIAG_MEMINFO_TESTING
#include "bb_diag_meminfo_test.h"

bb_err_t bb_diag_meminfo_assemble_schema_for_test(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return ensure_schema_patched();
#else
    return BB_OK;
#endif
}

const char *bb_diag_meminfo_get_describe_schema_for_test(void)
{
    return s_diag_meminfo_describe_responses[0].schema;
}
#endif /* BB_DIAG_MEMINFO_TESTING */
