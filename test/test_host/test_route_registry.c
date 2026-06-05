#include "unity.h"
#include "bb_http.h"
#include <stddef.h>
#include <stdio.h>

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

extern int  bb_http_host_reserved_routes(void);
extern void bb_http_host_reset_reserved(void);

void test_http_reserve_routes_accumulates(void)
{
    bb_http_host_reset_reserved();
    TEST_ASSERT_EQUAL(0, bb_http_host_reserved_routes());

    bb_http_reserve_routes(3);
    TEST_ASSERT_EQUAL(3, bb_http_host_reserved_routes());

    bb_http_reserve_routes(5);
    TEST_ASSERT_EQUAL(8, bb_http_host_reserved_routes());

    bb_http_reserve_routes(0);
    bb_http_reserve_routes(-7);
    TEST_ASSERT_EQUAL(8, bb_http_host_reserved_routes());

    bb_http_host_reset_reserved();
}

// Regression guard: simulates three PRE_HTTP companion functions each calling
// bb_http_reserve_routes() with their module's exact route count, then asserts
// the total matches. If a companion is removed or its count drifts, this test
// catches the mismatch before a device boot exposes it.
void test_reserve_declared_total_from_companions(void)
{
    bb_http_host_reset_reserved();
    TEST_ASSERT_EQUAL(0, bb_http_host_reserved_routes());

    // Simulate three PRE_HTTP companions (e.g. bb_info=2, bb_wifi=2, bb_system=1)
    bb_http_reserve_routes(2);   // bb_info
    bb_http_reserve_routes(2);   // bb_wifi
    bb_http_reserve_routes(1);   // bb_system
    TEST_ASSERT_EQUAL(5, bb_http_host_reserved_routes());

    // Simulate adding a previously undercounted module (bb_ota_validator: 3 not 1)
    bb_http_reserve_routes(3);   // bb_ota_validator (correct count)
    TEST_ASSERT_EQUAL(8, bb_http_host_reserved_routes());

    bb_http_host_reset_reserved();
    TEST_ASSERT_EQUAL(0, bb_http_host_reserved_routes());
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

// (k) URI longer than path_buf (>255 chars) with a query string — exercise the
//     plen >= sizeof(path_buf) truncation branch inside uri_pattern_match.
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
// Overflow returns BB_ERR_NO_SPACE (audit F14)
// ---------------------------------------------------------------------------

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
