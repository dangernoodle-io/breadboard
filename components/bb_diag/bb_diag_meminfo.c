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
// `static const` bb_route_t (handler=NULL), .rodata/flash, never DRAM. See
// bb_diag_section_t.describe_route's doc comment
// (components/bb_diag/include/bb_diag_section.h) for the full mechanism.
// Lives here (the section's registrar) rather than in
// bb_meminfo_heap_snap.c -- that file's own header (bb_meminfo_heap_snap.h)
// is a genuinely public bb_meminfo surface and must not gain a bb_route_t
// dependency; the schema literal it already exports
// (bb_meminfo_heap_snap_schema) is enough for this route.
// ---------------------------------------------------------------------------

static const bb_route_response_t s_diag_meminfo_describe_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = BB_MEMINFO_HEAP_SNAP_SCHEMA_LITERAL },
    { .status = 0 },
};

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
