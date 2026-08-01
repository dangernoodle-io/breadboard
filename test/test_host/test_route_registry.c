#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Minimal handler stub — not invoked by registry tests
// ---------------------------------------------------------------------------
static bb_err_t stub_handler(bb_http_request_t *req)
{
    (void)req;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Static descriptors — lifetime must outlast the test
// ---------------------------------------------------------------------------
static const bb_route_response_t s_stats_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
    { .status = 0 },
};

static const bb_route_t s_route_stats = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/stats",
    .tag                  = "mining",
    .summary              = "Get mining statistics",
    .operation_id         = "getStats",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_stats_responses,
    .handler              = stub_handler,
};

static const bb_route_response_t s_health_responses[] = {
    { .status = 200, .content_type = "text/plain", .schema = NULL, .description = "healthy" },
    { .status = 0 },
};

static const bb_route_t s_route_health = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/health",
    .tag                  = "system",
    .summary              = "Health check",
    .operation_id         = NULL,
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_health_responses,
    .handler              = stub_handler,
};

// ---------------------------------------------------------------------------
// Walker helper — collects visited pointers
// ---------------------------------------------------------------------------
#define WALK_CAP 16

typedef struct {
    const bb_route_t *visited[WALK_CAP];
    size_t            count;
} walk_ctx_t;

static void collect_walker(const bb_route_t *route, void *ctx)
{
    walk_ctx_t *wc = (walk_ctx_t *)ctx;
    if (wc->count < WALK_CAP) {
        wc->visited[wc->count++] = route;
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_route_registry_count_starts_at_zero(void)
{
    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(0, bb_http_route_registry_count());
}

void test_route_registry_add_increments_count(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());
}

void test_route_registry_add_two_increments_count(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    bb_http_register_described_route(NULL, &s_route_health);
    TEST_ASSERT_EQUAL(2, bb_http_route_registry_count());
}

void test_route_registry_foreach_visits_all(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    bb_http_register_described_route(NULL, &s_route_health);

    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);

    TEST_ASSERT_EQUAL(2, ctx.count);
    TEST_ASSERT_EQUAL_PTR(&s_route_stats,  ctx.visited[0]);
    TEST_ASSERT_EQUAL_PTR(&s_route_health, ctx.visited[1]);
}

void test_route_registry_foreach_preserves_insertion_order(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_health);
    bb_http_register_described_route(NULL, &s_route_stats);

    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);

    TEST_ASSERT_EQUAL_PTR(&s_route_health, ctx.visited[0]);
    TEST_ASSERT_EQUAL_PTR(&s_route_stats,  ctx.visited[1]);
}

void test_route_registry_clear_empties_registry(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    bb_http_register_described_route(NULL, &s_route_health);
    TEST_ASSERT_EQUAL(2, bb_http_route_registry_count());

    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(0, bb_http_route_registry_count());
}

void test_route_registry_foreach_empty_is_noop(void)
{
    bb_http_route_registry_clear();

    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);

    TEST_ASSERT_EQUAL(0, ctx.count);
}

void test_route_registry_foreach_null_cb_is_safe(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    // must not crash
    bb_http_route_registry_foreach(NULL, NULL);
}

void test_route_registry_descriptor_fields_preserved(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);

    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);

    TEST_ASSERT_EQUAL(1, ctx.count);
    const bb_route_t *r = ctx.visited[0];
    TEST_ASSERT_EQUAL(BB_HTTP_GET, r->method);
    TEST_ASSERT_EQUAL_STRING("/api/stats", r->path);
    TEST_ASSERT_EQUAL_STRING("mining", r->tag);
    TEST_ASSERT_EQUAL_STRING("Get mining statistics", r->summary);
    TEST_ASSERT_EQUAL_STRING("getStats", r->operation_id);
    TEST_ASSERT_NULL(r->request_content_type);
    TEST_ASSERT_NULL(r->request_schema);
    TEST_ASSERT_NOT_NULL(r->responses);
    TEST_ASSERT_EQUAL(200, r->responses[0].status);
    TEST_ASSERT_EQUAL(0,   r->responses[1].status);
}

void test_route_registry_count_after_clear_and_readd(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_health);
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());
}

// Forward declaration for test infra
extern void bb_http_host_force_register_fail(bool fail);

void test_register_described_route_rejects_null(void)
{
    bb_http_route_registry_clear();
    bb_err_t err = bb_http_register_described_route(NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(0, bb_http_route_registry_count());
}

void test_register_described_route_propagates_underlying_failure(void)
{
    bb_http_route_registry_clear();
    bb_http_host_force_register_fail(true);
    bb_err_t err = bb_http_register_described_route(NULL, &s_route_stats);
    bb_http_host_force_register_fail(false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(0, bb_http_route_registry_count());
}

void test_register_described_route_overflow_returns_no_space(void)
{
    bb_http_route_registry_clear();
    // Every one of these 65 descriptors is a real (non-NULL-handler) /api/*
    // route, so bb_http_register_described_route() also drives them through
    // bb_dispatch_api_add() (route_registry.c) — a process-wide table shared
    // with every other test in this binary. This test's 64 successful
    // registrations, followed by a genuine 65th-overflow, depend on the
    // table's actual occupancy (B1-1253: bb_http_register_route now
    // propagates a real dispatch-table failure instead of always swallowing
    // it to BB_OK) -- isolation comes from the global setUp() (test_main.c),
    // which resets the dispatch table before every test in this binary.

    // Build 65 route descriptors (cap is 64)
    static const bb_route_response_t s_overflow_responses[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };

    static bb_route_t s_overflow_routes[65];
    for (int i = 0; i < 65; i++) {
        // Build path string: "/api/r0", "/api/r1", ..., "/api/r64"
        static char paths[65][16];
        snprintf(paths[i], sizeof(paths[i]), "/api/r%d", i);
        s_overflow_routes[i] = (bb_route_t){
            .method               = BB_HTTP_GET,
            .path                 = paths[i],
            .tag                  = "overflow",
            .summary              = "overflow test",
            .operation_id         = NULL,
            .request_content_type = NULL,
            .request_schema       = NULL,
            .responses            = s_overflow_responses,
            .handler              = stub_handler,
        };
    }

    // Register 64 routes — should succeed
    for (int i = 0; i < 64; i++) {
        bb_err_t err = bb_http_register_described_route(NULL, &s_overflow_routes[i]);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());

    // 65th must return BB_ERR_NO_SPACE so the caller can detect the drop
    bb_err_t err = bb_http_register_described_route(NULL, &s_overflow_routes[64]);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());
}

// ---------------------------------------------------------------------------
// bb_http_register_described_route with route->handler == NULL (B1-452):
// schema-only registration — the descriptor is added to the registry but the
// httpd/dispatch handler-registration step is skipped entirely.
// ---------------------------------------------------------------------------

static const bb_route_response_t s_null_handler_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
    { .status = 0 },
};

static const bb_route_t s_route_null_handler = {
    .method    = BB_HTTP_GET,
    .path      = "/api/schema-only",
    .tag       = "test",
    .summary   = "schema-only route",
    .responses = s_null_handler_responses,
    .handler   = NULL,
};

void test_register_described_route_null_handler_adds_descriptor(void)
{
    bb_http_route_registry_clear();
    bb_err_t err = bb_http_register_described_route(NULL, &s_route_null_handler);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());

    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);
    TEST_ASSERT_EQUAL(1, ctx.count);
    TEST_ASSERT_EQUAL_PTR(&s_route_null_handler, ctx.visited[0]);
    TEST_ASSERT_NULL(ctx.visited[0]->handler);
}

void test_register_described_route_null_handler_skips_dispatch(void)
{
    bb_http_route_registry_clear();

    bb_err_t err = bb_http_register_described_route(NULL, &s_route_null_handler);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // No handler was wired: a lookup for this path must miss the dispatch
    // table even though the descriptor is present in the OpenAPI registry.
    bb_http_handler_fn handler = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/schema-only", &handler);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
    TEST_ASSERT_NULL(handler);
}

void test_register_described_route_null_handler_ignores_force_register_fail(void)
{
    // The NULL-handler branch never calls bb_http_register_route, so the
    // "force underlying registration to fail" test hook must have no effect.
    bb_http_route_registry_clear();
    bb_http_host_force_register_fail(true);
    bb_err_t err = bb_http_register_described_route(NULL, &s_route_null_handler);
    bb_http_host_force_register_fail(false);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());
}

// ---------------------------------------------------------------------------
// bb_http_register_route_descriptor_only tests
// ---------------------------------------------------------------------------

void test_register_route_descriptor_only_rejects_null(void)
{
    bb_http_route_registry_clear();
    bb_err_t err = bb_http_register_route_descriptor_only(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(0, bb_http_route_registry_count());
}

void test_register_route_descriptor_only_adds_to_registry(void)
{
    bb_http_route_registry_clear();
    bb_err_t err = bb_http_register_route_descriptor_only(&s_route_stats);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());

    // Verify the descriptor is preserved in the registry
    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);
    TEST_ASSERT_EQUAL(1, ctx.count);
    TEST_ASSERT_EQUAL_PTR(&s_route_stats, ctx.visited[0]);
}

void test_register_route_descriptor_only_overflow_returns_no_space(void)
{
    bb_http_route_registry_clear();

    static const bb_route_response_t s_overflow_responses[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };

    static bb_route_t s_overflow_routes[65];
    for (int i = 0; i < 65; i++) {
        static char paths[65][16];
        snprintf(paths[i], sizeof(paths[i]), "/api/do%d", i);
        s_overflow_routes[i] = (bb_route_t){
            .method    = BB_HTTP_GET,
            .path      = paths[i],
            .tag       = "overflow",
            .summary   = "descriptor-only overflow test",
            .responses = s_overflow_responses,
            .handler   = stub_handler,
        };
    }

    for (int i = 0; i < 64; i++) {
        bb_err_t err = bb_http_register_route_descriptor_only(&s_overflow_routes[i]);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());

    // 65th must return BB_ERR_NO_SPACE so the caller can detect the drop
    bb_err_t err = bb_http_register_route_descriptor_only(&s_overflow_routes[64]);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());
}

void test_register_route_descriptor_only_overflow_logs_null_path(void)
{
    bb_http_route_registry_clear();

    // Build 64 route descriptors with non-NULL paths to fill the registry
    static const bb_route_response_t s_overflow_responses[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };

    static bb_route_t s_overflow_routes[64];
    for (int i = 0; i < 64; i++) {
        // Build path string: "/api/r0", "/api/r1", ..., "/api/r63"
        static char paths[64][16];
        snprintf(paths[i], sizeof(paths[i]), "/api/r%d", i);
        s_overflow_routes[i] = (bb_route_t){
            .method               = BB_HTTP_GET,
            .path                 = paths[i],
            .tag                  = "overflow",
            .summary              = "overflow test",
            .operation_id         = NULL,
            .request_content_type = NULL,
            .request_schema       = NULL,
            .responses            = s_overflow_responses,
            .handler              = stub_handler,
        };
    }

    // Register 64 routes to fill the registry
    for (int i = 0; i < 64; i++) {
        bb_err_t err = bb_http_register_described_route(NULL, &s_overflow_routes[i]);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());

    // Register one more with NULL path; should succeed and exercise the ternary
    static const bb_route_t s_null_path_route = {
        .method               = BB_HTTP_GET,
        .path                 = NULL,
        .tag                  = "null-test",
        .summary              = "null path test",
        .operation_id         = NULL,
        .request_content_type = NULL,
        .request_schema       = NULL,
        .responses            = s_overflow_responses,
        .handler              = stub_handler,
    };

    bb_err_t err = bb_http_register_described_route(NULL, &s_null_path_route);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());
}

// ---------------------------------------------------------------------------
// bb_http_route_handler_count/cap tests
// ---------------------------------------------------------------------------

void test_http_route_handler_count_returns_zero_on_host(void)
{
    // On host, these always return 0 (no real httpd running)
    TEST_ASSERT_EQUAL(0, bb_http_route_handler_count());
    TEST_ASSERT_EQUAL(0, bb_http_route_handler_cap());
}

void test_register_route_table_registers_all(void)
{
    bb_http_route_registry_clear();
    const bb_route_t * const table[] = { &s_route_stats, &s_route_health };
    bb_err_t err = bb_http_register_route_table(NULL, table, 2);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, bb_http_route_registry_count());
}

void test_register_route_table_null_table_returns_err(void)
{
    bb_err_t err = bb_http_register_route_table(NULL, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_register_route_table_propagates_failure(void)
{
    bb_http_route_registry_clear();
    bb_http_host_force_register_fail(true);
    const bb_route_t * const table[] = { &s_route_stats, &s_route_health };
    bb_err_t err = bb_http_register_route_table(NULL, table, 2);
    bb_http_host_force_register_fail(false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    // First entry's described-route call failed; nothing was added.
    TEST_ASSERT_EQUAL(0, bb_http_route_registry_count());
}

// ---------------------------------------------------------------------------
// bb_http_uri_is_registered
// ---------------------------------------------------------------------------

// Reusable descriptors for the predicate tests (POST-only route + wildcard)
static const bb_route_response_t s_apply_responses[] = {
    { .status = 202, .content_type = "application/json", .schema = NULL, .description = "started" },
    { .status = 0 },
};

static const bb_route_t s_route_apply = {
    .method    = BB_HTTP_POST,
    .path      = "/api/update/apply",
    .tag       = "ota",
    .summary   = "Trigger OTA update",
    .responses = s_apply_responses,
    .handler   = stub_handler,
};

static const bb_route_response_t s_wildcard_responses[] = {
    { .status = 200, .content_type = "text/event-stream", .schema = NULL, .description = "stream" },
    { .status = 0 },
};

// A real wildcard route that IS registered (e.g. /api/events/*)
static const bb_route_t s_route_events_wildcard = {
    .method    = BB_HTTP_GET,
    .path      = "/api/events/*",
    .tag       = "events",
    .summary   = "SSE stream",
    .responses = s_wildcard_responses,
    .handler   = stub_handler,
};

// The internal asset catch-all — same path as OPTIONS preflight but different method
static const bb_route_t s_route_asset_catchall = {
    .method    = BB_HTTP_GET,
    .path      = "/*",
    .tag       = "assets",
    .summary   = "asset wildcard",
    .responses = s_wildcard_responses,
    .handler   = stub_handler,
};

// The internal OPTIONS preflight catch-all
static const bb_route_t s_route_preflight_catchall = {
    .method    = BB_HTTP_OPTIONS,
    .path      = "/*",
    .tag       = "cors",
    .summary   = "preflight wildcard",
    .responses = s_wildcard_responses,
    .handler   = stub_handler,
};

// (a) Exact-match registered route → true
void test_uri_is_registered_exact_match(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    TEST_ASSERT_TRUE(bb_http_uri_is_registered("/api/stats"));
}

// (b) Unregistered path → false
void test_uri_is_registered_bogus_path(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    TEST_ASSERT_FALSE(bb_http_uri_is_registered("/api/totally-bogus-xyz"));
}

// (c) POST-only route; URI is registered (just for wrong method — 405 is correct)
void test_uri_is_registered_post_only_route(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_apply);
    // URI IS registered (as POST) — predicate should return true
    TEST_ASSERT_TRUE(bb_http_uri_is_registered("/api/update/apply"));
}

// (d) Wildcard route matches sub-paths
void test_uri_is_registered_wildcard_route_matches_subpath(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_events_wildcard);
    TEST_ASSERT_TRUE(bb_http_uri_is_registered("/api/events/mining"));
}

// (e) Wildcard route registered; unrelated bogus path → false
void test_uri_is_registered_wildcard_route_does_not_match_unrelated(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_events_wildcard);
    TEST_ASSERT_FALSE(bb_http_uri_is_registered("/api/totally-bogus-xyz"));
}

// (f) GET /* (asset catch-all) and OPTIONS /* (preflight) in registry — should
//     not make every path "registered". These are the internal catch-alls that
//     the real bb_http server registers outside the described-route path; if
//     they somehow end up in the registry the predicate must exclude them.
void test_uri_is_registered_catchall_wildcards_excluded(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_asset_catchall);
    bb_http_register_described_route(NULL, &s_route_preflight_catchall);
    // A bogus path must not be considered registered just because "/*" matches
    TEST_ASSERT_FALSE(bb_http_uri_is_registered("/api/totally-bogus-xyz"));
}

// (g) Query string stripped before matching
void test_uri_is_registered_query_string_stripped(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    TEST_ASSERT_TRUE(bb_http_uri_is_registered("/api/stats?format=json"));
}

// (h) NULL uri → false (no crash)
void test_uri_is_registered_null_uri(void)
{
    bb_http_route_registry_clear();
    TEST_ASSERT_FALSE(bb_http_uri_is_registered(NULL));
}

// (i) Empty registry → false
void test_uri_is_registered_empty_registry(void)
{
    bb_http_route_registry_clear();
    TEST_ASSERT_FALSE(bb_http_uri_is_registered("/api/stats"));
}

// (j) Route with null path in registry — skip over it, not crash
void test_uri_is_registered_skips_null_path_entry(void)
{
    bb_http_route_registry_clear();
    // Register a route with null path via descriptor-only (bypasses the
    // httpd layer which would reject it), then follow it with a real route.
    static const bb_route_response_t s_resp[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };
    static const bb_route_t s_null_path = {
        .method    = BB_HTTP_GET,
        .path      = NULL,
        .tag       = "test",
        .summary   = "null path",
        .responses = s_resp,
        .handler   = stub_handler,
    };
    static const bb_route_t s_real = {
        .method    = BB_HTTP_GET,
        .path      = "/api/real",
        .tag       = "test",
        .summary   = "real route",
        .responses = s_resp,
        .handler   = stub_handler,
    };
    // Use descriptor-only to place the null-path entry directly into the registry
    bb_http_register_route_descriptor_only(&s_null_path);
    bb_http_register_route_descriptor_only(&s_real);
    // Must not crash, and the real route must still match
    TEST_ASSERT_TRUE(bb_http_uri_is_registered("/api/real"));
    TEST_ASSERT_FALSE(bb_http_uri_is_registered("/api/bogus"));
}

// (k) URI longer than 255 chars with a query string, compared against an
//     unrelated short pattern — must not crash and must not match (different
//     path). This no longer exercises any internal-buffer truncation branch
//     (bb_http_uri_is_registered was folded onto the shared bb_route_uri_match
//     seam, which is unbounded via match_upto -- see
//     test_uri_is_registered_long_path_beyond_old_256_byte_buffer_matches
//     below for the truncation-bug regression test).
void test_uri_is_registered_very_long_uri_with_query(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);

    // Build a URI whose path part is 300 'a' chars followed by "?q=1".
    // snprintf fills 300 'a's then the query string.
    char long_uri[310];
    snprintf(long_uri, sizeof(long_uri),
             "%300s?q=1",
             "/api/stats");  // snprintf pads with spaces; we overwrite below
    // Overwrite the space-padding with 'a' up to the '?'
    for (int i = 0; i < 300 && long_uri[i] != '?'; i++) {
        long_uri[i] = 'a';
    }
    // Should not match /api/stats (different path), and must not crash
    TEST_ASSERT_FALSE(bb_http_uri_is_registered(long_uri));
}

// Pin the fix for a real pre-existing bug: uri_pattern_match's now-deleted
// 256-byte path_buf silently truncated the query-stripped uri before
// comparing, so a route whose own path exceeds 255 bytes could never be
// found as "registered" once a query string pushed the copy through
// truncation -- bb_http_uri_is_registered would 404 a request that
// bb_dispatch_api_lookup (already unbounded via match_upto, never routed
// through uri_pattern_match) would have HIT, a genuine 404-vs-405
// disambiguation divergence between the two. Folding onto the shared
// bb_route_uri_match seam (unbounded, memcmp against match_upto) removes
// the truncation entirely. This is a behavior CHANGE, not a coincidental
// side effect of the refactor -- see the commit body.
void test_uri_is_registered_long_path_beyond_old_256_byte_buffer_matches(void)
{
    bb_http_route_registry_clear();

    // A 300-byte exact-match path -- longer than the old path_buf[256].
    static char long_path[301];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    static const bb_route_response_t s_resp[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };
    static const bb_route_t s_long_route = {
        .method    = BB_HTTP_GET,
        .path      = long_path,
        .tag       = "test",
        .summary   = "long path",
        .responses = s_resp,
        .handler   = stub_handler,
    };
    bb_http_register_route_descriptor_only(&s_long_route);

    // Same 300-byte path plus a query string -- under the old
    // 256-byte-truncating uri_pattern_match this would have been silently
    // clipped to 255 bytes before comparison and returned FALSE (a false
    // 404). It must now return TRUE.
    char long_uri[310];
    memcpy(long_uri, long_path, sizeof(long_path) - 1);
    memcpy(long_uri + (sizeof(long_path) - 1), "?q=1", 5);
    TEST_ASSERT_TRUE(bb_http_uri_is_registered(long_uri));
}

// (l) Pattern with empty path — exercise plen==0 branch in uri_pattern_match.
void test_uri_is_registered_empty_pattern_no_match(void)
{
    bb_http_route_registry_clear();
    static const bb_route_response_t s_resp[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };
    static const bb_route_t s_empty_path = {
        .method    = BB_HTTP_GET,
        .path      = "",
        .tag       = "test",
        .summary   = "empty path",
        .responses = s_resp,
        .handler   = stub_handler,
    };
    bb_http_register_route_descriptor_only(&s_empty_path);
    // Empty pattern matches only the empty URI (exact strcmp)
    TEST_ASSERT_TRUE(bb_http_uri_is_registered(""));
    TEST_ASSERT_FALSE(bb_http_uri_is_registered("/api/anything"));
}

// (m) Non-GET, non-OPTIONS route with path "/*" — exercises the false branch of
//     the GET /* exclusion: path matches but method is neither GET nor OPTIONS so
//     the route IS considered registered (it contributes to 405 logic).
void test_uri_is_registered_post_catchall_is_registered(void)
{
    bb_http_route_registry_clear();
    static const bb_route_response_t s_resp[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };
    static const bb_route_t s_post_catchall = {
        .method    = BB_HTTP_POST,
        .path      = "/*",
        .tag       = "test",
        .summary   = "post catchall",
        .responses = s_resp,
        .handler   = stub_handler,
    };
    bb_http_register_route_descriptor_only(&s_post_catchall);
    // POST /* is NOT the asset or preflight wildcard, so it IS considered
    // registered — any path matches it via wildcard.
    TEST_ASSERT_TRUE(bb_http_uri_is_registered("/api/anything"));
}

// ---------------------------------------------------------------------------
// Regression: described-route with static-storage descriptor survives registry
// walk after registration (guards the 405-walk dangling-pointer fix).
//
// Contract: callers pass a descriptor whose lifetime outlasts the registry.
// We cannot unit-test UB directly, so we assert positive coverage:
//   - is_registered returns true for the registered path
//   - is_registered returns false for an unregistered path
//   - foreach yields a pointer whose .path field is readable (non-crashing)
// If the fix ever regresses to a stack-local, Address Sanitizer or Valgrind
// will catch the stale-pointer read here before it crashes on device.
// ---------------------------------------------------------------------------

// Static descriptors that mirror the pattern used by bb_thermal (handler
// wired at registration time, not in the initialiser — hence they cannot
// be const).
static const bb_route_response_t s_persist_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
    { .status = 0 },
};

// Template (const): all fields except handler.
static const bb_route_t s_persist_template = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/persist-test",
    .tag                  = "test",
    .summary              = "persist regression",
    .operation_id         = "getPersistTest",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_persist_responses,
    .handler              = NULL,
};

// Working copy with static storage: mirrors bb_thermal fix.
static bb_route_t s_persist_working;

void test_uri_is_registered_described_route_persists_in_registry(void)
{
    bb_http_route_registry_clear();

    // Wire handler at "init time" (same pattern as the fixed components).
    s_persist_working         = s_persist_template;
    s_persist_working.handler = stub_handler;

    bb_err_t err = bb_http_register_described_route(NULL, &s_persist_working);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Positive: path IS registered.
    TEST_ASSERT_TRUE(bb_http_uri_is_registered("/api/persist-test"));

    // Negative: unrelated path is NOT registered.
    TEST_ASSERT_FALSE(bb_http_uri_is_registered("/api/not-registered"));

    // foreach must yield a valid (non-crashing) path string — if the descriptor
    // were a dangling stack pointer, ASan/Valgrind would fire here.
    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);
    TEST_ASSERT_EQUAL(1, ctx.count);
    TEST_ASSERT_NOT_NULL(ctx.visited[0]->path);
    TEST_ASSERT_EQUAL_STRING("/api/persist-test", ctx.visited[0]->path);
}

// ---------------------------------------------------------------------------
// bb_http_register_route /api/* error handling (B1-1253): a duplicate
// (method,path) is the intentionally-tolerated case and must still return
// BB_OK (first registration wins, e.g. bb_wifi_prov racing bb_wifi_http for
// the same path); a genuine dispatch-table failure (BB_ERR_NO_SPACE) must
// now propagate instead of being silently swallowed to BB_OK.
// ---------------------------------------------------------------------------

// (a) A duplicate (method,path) registration is swallowed to BB_OK — the
// intentionally-tolerated case must not become a hard error for a caller
// that doesn't special-case it.
void test_register_route_api_duplicate_swallowed_to_ok(void)
{
    bb_err_t first = bb_http_register_route(NULL, BB_HTTP_POST,
                                            "/api/wifi/scan", stub_handler);
    TEST_ASSERT_EQUAL(BB_OK, first);

    bb_err_t dup = bb_http_register_route(NULL, BB_HTTP_POST,
                                          "/api/wifi/scan", stub_handler);
    TEST_ASSERT_EQUAL(BB_OK, dup);

    // First registration wins: the table still holds exactly one entry.
    TEST_ASSERT_EQUAL(1, bb_dispatch_api_count());
}

// (b) A genuine dispatch-table failure (table full) must propagate as a real
// error, not be swallowed to BB_OK — this is the defect B1-1253 fixes.
void test_register_route_api_dispatch_full_propagates_no_space(void)
{
    static char fill_paths[BB_DISPATCH_API_CAP][24];
    for (int i = 0; i < BB_DISPATCH_API_CAP; i++) {
        snprintf(fill_paths[i], sizeof(fill_paths[i]), "/api/fill%d", i);
        bb_err_t err = bb_http_register_route(NULL, BB_HTTP_GET,
                                              fill_paths[i], stub_handler);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_CAP, (int)bb_dispatch_api_count());

    bb_err_t err = bb_http_register_route(NULL, BB_HTTP_GET,
                                          "/api/one-too-many", stub_handler);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_CAP, (int)bb_dispatch_api_count());
}

// (c) Non-/api/* paths are unaffected by this change (bypass the dispatch
// table entirely on host).
void test_register_route_non_api_path_unaffected(void)
{
    bb_err_t err = bb_http_register_route(NULL, BB_HTTP_POST,
                                          "/save", stub_handler);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

// (d) Cross-component scenario the design decision at bb_http.c's dup-vs-
// genuine-failure branch rests on: bb_wifi_prov registers POST
// /api/wifi/scan via the plain bb_http_register_route() call shape
// (bb_wifi_prov.c:351), while bb_wifi_http registers the SAME path via a
// bb_route_t descriptor through bb_http_register_described_route()
// (bb_wifi_http_routes.c:364, which treats any non-BB_OK as fatal for its
// entire bb_wifi_routes_init() — it does NOT special-case
// BB_ERR_INVALID_STATE). Composing both in the SAME process (as a live
// firmware can) must not turn the second registration into a hard error,
// regardless of which of the two components happens to register first.
static const bb_route_response_t s_scan_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
    { .status = 0 },
};

static const bb_route_t s_route_scan_described = {
    .method    = BB_HTTP_POST,
    .path      = "/api/wifi/scan",
    .tag       = "wifi",
    .summary   = "scan wifi",
    .responses = s_scan_responses,
    .handler   = stub_handler,
};

void test_register_route_cross_component_scan_dup_bb_wifi_prov_then_bb_wifi_http(void)
{
    bb_http_route_registry_clear();

    // bb_wifi_prov's call shape: plain bb_http_register_route().
    bb_err_t prov_err = bb_http_register_route(NULL, BB_HTTP_POST,
                                               "/api/wifi/scan", stub_handler);
    TEST_ASSERT_EQUAL(BB_OK, prov_err);

    // bb_wifi_http's call shape: described-route, and it is fatal on ANY
    // non-BB_OK return -- this assertion is the enforced invariant.
    bb_err_t http_err = bb_http_register_described_route(NULL, &s_route_scan_described);
    TEST_ASSERT_EQUAL(BB_OK, http_err);

    TEST_ASSERT_EQUAL(1, bb_dispatch_api_count());
}

void test_register_route_cross_component_scan_dup_bb_wifi_http_then_bb_wifi_prov(void)
{
    bb_http_route_registry_clear();

    // Reverse composition order: bb_wifi_http registers first.
    bb_err_t http_err = bb_http_register_described_route(NULL, &s_route_scan_described);
    TEST_ASSERT_EQUAL(BB_OK, http_err);

    bb_err_t prov_err = bb_http_register_route(NULL, BB_HTTP_POST,
                                               "/api/wifi/scan", stub_handler);
    TEST_ASSERT_EQUAL(BB_OK, prov_err);

    TEST_ASSERT_EQUAL(1, bb_dispatch_api_count());
}

// ---------------------------------------------------------------------------
// Overflow returns BB_ERR_NO_SPACE (audit F14)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Route-registry-cap fold (B1-1319): the overflow-strict escalation and the
// high-watermark warn used to live in bb_http_route_audit_init(), a
// bbtool:init composition entry that codegen always emitted BEFORE every
// route registration (it has no `order=`-solvable dependency edge on the
// routes it's meant to audit), so it always audited a near-empty registry
// and never fired. Folded into registry_add() itself, both behaviors are
// now order-independent -- they run at the registration that actually
// crosses the threshold.
// ---------------------------------------------------------------------------

extern bool bb_http_route_registry_strict_would_fire(void);
extern bool bb_http_route_registry_strict_trap_hit(void);
extern void bb_http_route_registry_strict_trap_reset(void);
extern void bb_http_route_registry_strict_force(bool enabled);
extern void bb_http_route_registry_strict_force_reset(void);

// The STRICT decision (CONFIG_BB_HTTP_ROUTE_REGISTRY_STRICT, Kconfig default
// y) drives a process-aborting assert() on host, which a Unity test cannot
// observe as a passing result (BB_HTTP_TESTING compiles the assert() itself
// out -- see registry_add()). Pinning the predicate alone only proves the
// Kconfig default resolves to true in isolation -- it says nothing about
// whether registry_add() actually wires that predicate into the overflow
// path (a reviewer confirmed the escalation block could be deleted wholesale
// without failing a single native test under the predicate-only version of
// this test). This test instead drives a REAL overflow through
// registry_add() and asserts the trap hook (set inside the escalation
// block) actually fired -- so removing or short-circuiting that block
// fails this test, not just an isolated predicate check. The trap's
// setter/reset function SHAPE echoes bb_http_host_force_register_fail(),
// but the trap lives in the portable, always-compiled route_registry.c
// (guarded #ifdef BB_HTTP_TESTING) rather than in a host-only TU, since
// registry_add() itself needs to set it.
void test_route_registry_strict_trap_fires_on_overflow(void)
{
    // bb_http_route_registry_clear() resets the trap/override test seams
    // too (see its own doc comment in route_registry.c).
    bb_http_route_registry_clear();
    TEST_ASSERT_FALSE(bb_http_route_registry_strict_trap_hit());

    static const bb_route_response_t s_resp[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };
    static bb_route_t s_routes[65];
    static char       s_paths[65][16];
    for (int i = 0; i < 65; i++) {
        snprintf(s_paths[i], sizeof(s_paths[i]), "/api/trap%d", i);
        s_routes[i] = (bb_route_t){
            .method    = BB_HTTP_GET,
            .path      = s_paths[i],
            .tag       = "trap",
            .summary   = "strict trap test",
            .responses = s_resp,
            .handler   = stub_handler,
        };
    }

    // Fill to cap -- trap must not have fired yet (no overflow reached).
    for (int i = 0; i < 64; i++) {
        bb_err_t err = bb_http_register_route_descriptor_only(&s_routes[i]);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_FALSE(bb_http_route_registry_strict_trap_hit());

    // 65th registration overflows -- with STRICT on (host default), the
    // escalation block in registry_add() must actually run.
    bb_err_t overflow_err = bb_http_register_route_descriptor_only(&s_routes[64]);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, overflow_err);
    TEST_ASSERT_TRUE(bb_http_route_registry_strict_trap_hit());

    bb_http_route_registry_strict_trap_reset();
}

// The Kconfig default resolves the STRICT decision the same way on every
// host test run, so without an override the false branch of
// "if (bb_http_route_registry_strict_would_fire())" in registry_add() is
// unreachable from a host test. bb_http_route_registry_strict_force()
// flips the decision for this test only, proving overflow with STRICT off
// still returns BB_ERR_NO_SPACE (fail-closed) but does NOT trip the trap
// hook -- the complementary case to
// test_route_registry_strict_trap_fires_on_overflow above.
void test_route_registry_strict_trap_does_not_fire_when_strict_disabled(void)
{
    bb_http_route_registry_clear();
    bb_http_route_registry_strict_force(false);

    static const bb_route_response_t s_resp[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };
    static bb_route_t s_routes[65];
    static char       s_paths[65][16];
    for (int i = 0; i < 65; i++) {
        snprintf(s_paths[i], sizeof(s_paths[i]), "/api/nostrict%d", i);
        s_routes[i] = (bb_route_t){
            .method    = BB_HTTP_GET,
            .path      = s_paths[i],
            .tag       = "nostrict",
            .summary   = "strict-disabled overflow test",
            .responses = s_resp,
            .handler   = stub_handler,
        };
    }

    for (int i = 0; i < 64; i++) {
        bb_err_t err = bb_http_register_route_descriptor_only(&s_routes[i]);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    bb_err_t overflow_err = bb_http_register_route_descriptor_only(&s_routes[64]);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, overflow_err);
    TEST_ASSERT_FALSE(bb_http_route_registry_strict_trap_hit());

    bb_http_route_registry_strict_force_reset();
    bb_http_route_registry_strict_trap_reset();
}

// High-watermark warn fires once when the count crosses CAP-8, mirroring
// bb_dispatch_api_add()'s s_dispatch_warned one-shot latch. There is no
// host-side log-capture seam (same limitation noted in
// test_bb_task_registry.c), so this test exercises both the true branch
// (first crossing) and the false branch (already-warned) without crashing,
// and pins that registrations keep succeeding all the way to cap.
void test_route_registry_watermark_warn_fires_once_at_cap_minus_8(void)
{
    bb_http_route_registry_clear();

    static const bb_route_response_t s_resp[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };
    static bb_route_t s_routes[64];
    static char       s_paths[64][16];

    for (int i = 0; i < 64; i++) {
        snprintf(s_paths[i], sizeof(s_paths[i]), "/api/wm%d", i);
        s_routes[i] = (bb_route_t){
            .method    = BB_HTTP_GET,
            .path      = s_paths[i],
            .tag       = "watermark",
            .summary   = "watermark test",
            .responses = s_resp,
            .handler   = stub_handler,
        };
    }

    // Registrations 0..55 (56 = CAP-8) stay below the watermark.
    for (int i = 0; i < 56; i++) {
        bb_err_t err = bb_http_register_route_descriptor_only(&s_routes[i]);
        TEST_ASSERT_EQUAL_MESSAGE(BB_OK, err, "expected BB_OK below watermark");
    }
    TEST_ASSERT_EQUAL(56, bb_http_route_registry_count());

    // Registration 56 (count becomes 57 = CAP-8+1) crosses the watermark --
    // exercises the one-shot latch's true branch. Still succeeds (warn is
    // informational, not a rejection).
    bb_err_t crossing = bb_http_register_route_descriptor_only(&s_routes[56]);
    TEST_ASSERT_EQUAL(BB_OK, crossing);
    TEST_ASSERT_EQUAL(57, bb_http_route_registry_count());

    // Registrations 57..63 stay above the watermark -- exercises the
    // already-warned (false) branch of the latch on every subsequent add,
    // and all the way to cap (64) still succeed.
    for (int i = 57; i < 64; i++) {
        bb_err_t err = bb_http_register_route_descriptor_only(&s_routes[i]);
        TEST_ASSERT_EQUAL_MESSAGE(BB_OK, err, "expected BB_OK up to cap");
    }
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());
}

// ---------------------------------------------------------------------------
// bb_http_uri_strip_query_copy invalid-arg branch. This function's own
// tests otherwise all reach it indirectly via bb_http_req_uri() (see
// platform/host/bb_http_server/bb_http_host.c and
// platform/espidf/bb_http_server/bb_http.c), which never passes invalid
// args -- so the reject-invalid-args branch was never directly exercised.
// This coverage gap pre-dates B1-1319's edit but the touched-file rule
// (a file with pre-existing baseline gaps must have ALL of them closed
// once you touch it) requires closing it here.
// ---------------------------------------------------------------------------

void test_uri_strip_query_copy_rejects_null_uri(void)
{
    char out[16];
    bb_err_t err = bb_http_uri_strip_query_copy(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_uri_strip_query_copy_rejects_null_out(void)
{
    bb_err_t err = bb_http_uri_strip_query_copy("/api/stats", NULL, 16);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_uri_strip_query_copy_rejects_zero_cap(void)
{
    char out[16];
    bb_err_t err = bb_http_uri_strip_query_copy("/api/stats", out, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_uri_strip_query_copy_accepts_valid_args(void)
{
    char out[16];
    bb_err_t err = bb_http_uri_strip_query_copy("/api/stats?x=1", out, sizeof(out));
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("/api/stats", out);
}

void test_registry_overflow_returns_no_space(void)
{
    bb_http_route_registry_clear();

    static const bb_route_response_t s_resp[] = {
        { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
        { .status = 0 },
    };
    static bb_route_t s_routes[65];
    static char       s_paths[65][16];

    for (int i = 0; i < 65; i++) {
        snprintf(s_paths[i], sizeof(s_paths[i]), "/api/f14/%d", i);
        s_routes[i] = (bb_route_t){
            .method    = BB_HTTP_GET,
            .path      = s_paths[i],
            .tag       = "f14",
            .summary   = "overflow audit F14",
            .responses = s_resp,
            .handler   = stub_handler,
        };
    }

    // Fill registry to cap with descriptor-only (no server needed).
    for (int i = 0; i < 64; i++) {
        bb_err_t err = bb_http_register_route_descriptor_only(&s_routes[i]);
        TEST_ASSERT_EQUAL_MESSAGE(BB_OK, err, "expected BB_OK for entries 0-63");
    }
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());

    // 65th registration must return BB_ERR_NO_SPACE.
    bb_err_t err = bb_http_register_route_descriptor_only(&s_routes[64]);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    // Registry count must remain at cap — descriptor was not added.
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());
}
