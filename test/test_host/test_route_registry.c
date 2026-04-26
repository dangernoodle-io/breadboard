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

void test_register_described_route_overflow_returns_ok(void)
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

    // 65th should succeed but not add to registry (overflow logged)
    bb_err_t err = bb_http_register_described_route(NULL, &s_overflow_routes[64]);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(64, bb_http_route_registry_count());
}
