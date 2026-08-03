#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "../../components/bb_http_server/src/bb_route_exclude.h"

#include <stddef.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Minimal handler stub — not invoked by exclusion tests
// ---------------------------------------------------------------------------
static bb_err_t stub_handler(bb_http_request_t *req)
{
    (void)req;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Walker helper — collects visited pointers (mirrors test_route_registry.c)
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
// Reusable descriptors
// ---------------------------------------------------------------------------
static const bb_route_response_t s_resp[] = {
    { .status = 200, .content_type = "application/json", .schema = NULL, .description = "ok" },
    { .status = 0 },
};

static const bb_route_t s_route_diag = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/meminfo",
    .tag       = "diag",
    .summary   = "meminfo",
    .responses = s_resp,
    .handler   = stub_handler,
};

static const bb_route_t s_route_health = {
    .method    = BB_HTTP_GET,
    .path      = "/api/health",
    .tag       = "system",
    .summary   = "health",
    .responses = s_resp,
    .handler   = stub_handler,
};

static const bb_route_t s_route_diag_events = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/events",
    .tag       = "diag",
    .summary   = "events",
    .responses = s_resp,
    .handler   = stub_handler,
};

// ---------------------------------------------------------------------------
// bb_http_route_exclude(): argument validation
// ---------------------------------------------------------------------------

void test_route_exclude_rejects_null_path(void)
{
    bb_http_route_exclude_reset();
    bb_err_t err = bb_http_route_exclude(BB_HTTP_GET, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_route_exclude_rejects_non_api_wildcard(void)
{
    bb_http_route_exclude_reset();
    bb_err_t err = bb_http_route_exclude(BB_HTTP_GET, "/save/*");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// B1-1401 HIGH fix: a non-/api/ EXACT-match path must be rejected exactly
// like the wildcard case above, not silently admitted -- there is no choke
// point on a handler-bound non-/api/ route this primitive can hook (see
// bb_http_route_exclude()'s doc comment, bb_http_server.h).
void test_route_exclude_rejects_non_api_exact_path(void)
{
    bb_http_route_exclude_reset();
    bb_err_t err = bb_http_route_exclude(BB_HTTP_GET, "/ping");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_route_exclude_accepts_api_wildcard(void)
{
    bb_http_route_exclude_reset();
    bb_err_t err = bb_http_route_exclude(BB_HTTP_GET, "/api/diag/*");
    TEST_ASSERT_EQUAL(BB_OK, err);
}

// Empty path is the same class of caller error as NULL -- it can never
// match a real registered route, so it is rejected up front rather than
// silently consuming a table slot that would only surface a confusing
// "never matched" from bb_http_route_exclude_verify() later. This also
// exercises the plen > 0 branch of bb_route_match_is_wildcard() via
// rejection rather than acceptance.
void test_route_exclude_rejects_empty_path(void)
{
    bb_http_route_exclude_reset();
    bb_err_t err = bb_http_route_exclude(BB_HTTP_GET, "");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_route_exclude_cap_exhaustion_returns_no_space(void)
{
    bb_http_route_exclude_reset();

    static char paths[BB_HTTP_ROUTE_EXCLUDE_CAP][24];
    for (int i = 0; i < BB_HTTP_ROUTE_EXCLUDE_CAP; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/api/x%d", i);
        bb_err_t err = bb_http_route_exclude(BB_HTTP_GET, paths[i]);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    bb_err_t err = bb_http_route_exclude(BB_HTTP_GET, "/api/one-too-many");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

// ---------------------------------------------------------------------------
// Exact-match exclusion suppresses BOTH choke points
// ---------------------------------------------------------------------------

void test_route_exclude_exact_match_suppresses_registry_add(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    bb_err_t exc_err = bb_http_route_exclude(BB_HTTP_GET, "/api/diag/meminfo");
    TEST_ASSERT_EQUAL(BB_OK, exc_err);

    bb_err_t reg_err = bb_http_register_described_route(NULL, &s_route_diag);
    TEST_ASSERT_EQUAL(BB_OK, reg_err);

    // Benign no-op: nothing was actually added to the registry.
    TEST_ASSERT_EQUAL(0, bb_http_route_registry_count());

    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);
    TEST_ASSERT_EQUAL(0, ctx.count);
}

void test_route_exclude_exact_match_suppresses_dispatch_add(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    bb_err_t exc_err = bb_http_route_exclude(BB_HTTP_GET, "/api/diag/meminfo");
    TEST_ASSERT_EQUAL(BB_OK, exc_err);

    bb_err_t reg_err = bb_http_register_described_route(NULL, &s_route_diag);
    TEST_ASSERT_EQUAL(BB_OK, reg_err);

    // Excluded route absent from the dispatch table: lookup MISSes.
    bb_http_handler_fn handler = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/diag/meminfo", &handler);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
    TEST_ASSERT_NULL(handler);
}

// An unrelated route registered alongside the excluded one is unaffected.
void test_route_exclude_does_not_affect_other_routes(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    bb_err_t exc_err = bb_http_route_exclude(BB_HTTP_GET, "/api/diag/meminfo");
    TEST_ASSERT_EQUAL(BB_OK, exc_err);

    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_diag));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_health));

    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());

    bb_http_handler_fn handler = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/health", &handler);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res);
    TEST_ASSERT_EQUAL_PTR(stub_handler, handler);
}

// bb_http_register_route_descriptor_only() is the schema-only path
// (handler==NULL registrations, and bb_ws_server's own call shape) — must
// also honor the exclusion at registry_add().
void test_route_exclude_suppresses_descriptor_only_registration(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    bb_err_t exc_err = bb_http_route_exclude(BB_HTTP_GET, "/api/diag/meminfo");
    TEST_ASSERT_EQUAL(BB_OK, exc_err);

    bb_err_t reg_err = bb_http_register_route_descriptor_only(&s_route_diag);
    TEST_ASSERT_EQUAL(BB_OK, reg_err);
    TEST_ASSERT_EQUAL(0, bb_http_route_registry_count());
}

// ---------------------------------------------------------------------------
// Wildcard exclusion under /api/
// ---------------------------------------------------------------------------

void test_route_exclude_wildcard_suppresses_matching_subpaths(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    bb_err_t exc_err = bb_http_route_exclude(BB_HTTP_GET, "/api/diag/*");
    TEST_ASSERT_EQUAL(BB_OK, exc_err);

    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_diag));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_diag_events));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_health));

    // Both /api/diag/* subpaths suppressed; /api/health untouched.
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());

    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);
    TEST_ASSERT_EQUAL(1, ctx.count);
    TEST_ASSERT_EQUAL_PTR(&s_route_health, ctx.visited[0]);
}

// ---------------------------------------------------------------------------
// route_excluded() failing to fire — proves the mechanism actually gates
// (regression tripwire): a NULL exclusion (never populated) must leave
// registration completely unaffected.
// ---------------------------------------------------------------------------

void test_route_exclude_empty_table_does_not_suppress_anything(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_diag));
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());

    bb_http_handler_fn handler = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/diag/meminfo", &handler);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res);
}

// ---------------------------------------------------------------------------
// bb_http_route_exclude_verify(): the two-phase declare/verify split
// ---------------------------------------------------------------------------

void test_route_exclude_verify_passes_when_all_matched(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_http_route_exclude(BB_HTTP_GET, "/api/diag/meminfo"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_diag));

    TEST_ASSERT_EQUAL(BB_OK, bb_http_route_exclude_verify());
}

void test_route_exclude_verify_returns_not_found_for_unmatched_entry(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    // Typo'd path — never registered, so this entry never matches.
    TEST_ASSERT_EQUAL(BB_OK, bb_http_route_exclude(BB_HTTP_GET, "/api/diag/memifno"));

    bb_err_t err = bb_http_route_exclude_verify();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

// First-unmatched-entry ordering: an earlier matched entry must not mask a
// later unmatched one, and verify() must stop at the FIRST unmatched entry.
void test_route_exclude_verify_names_first_unmatched_entry(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_http_route_exclude(BB_HTTP_GET, "/api/diag/meminfo"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_route_exclude(BB_HTTP_GET, "/api/never/matches"));

    // Only the first entry's route is actually registered.
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_diag));

    bb_err_t err = bb_http_route_exclude_verify();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

// ---------------------------------------------------------------------------
// bb_http_route_exclude_reset()
// ---------------------------------------------------------------------------

void test_route_exclude_reset_clears_table(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_http_route_exclude(BB_HTTP_GET, "/api/diag/meminfo"));
    bb_http_route_exclude_reset();

    // Verify passes trivially on an empty table (nothing to have matched).
    TEST_ASSERT_EQUAL(BB_OK, bb_http_route_exclude_verify());

    // And the exclusion no longer applies: registration proceeds normally.
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_diag));
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());
}

// ---------------------------------------------------------------------------
// Method-qualification: an exclusion on GET must not suppress the same path
// under a different method.
// ---------------------------------------------------------------------------

static const bb_route_t s_route_diag_post = {
    .method    = BB_HTTP_POST,
    .path      = "/api/diag/meminfo",
    .tag       = "diag",
    .summary   = "meminfo POST variant",
    .responses = s_resp,
    .handler   = stub_handler,
};

void test_route_exclude_method_qualified_does_not_suppress_other_method(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_http_route_exclude(BB_HTTP_GET, "/api/diag/meminfo"));

    // GET is suppressed...
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_diag));
    // ...but POST at the same path is unaffected.
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_described_route(NULL, &s_route_diag_post));

    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());

    walk_ctx_t ctx = { .count = 0 };
    bb_http_route_registry_foreach(collect_walker, &ctx);
    TEST_ASSERT_EQUAL(1, ctx.count);
    TEST_ASSERT_EQUAL_PTR(&s_route_diag_post, ctx.visited[0]);
}

// ---------------------------------------------------------------------------
// route_excluded() internal helper: a path with a query-string-like suffix
// (defensive — route_excluded is always called with a bare route path, but
// pin the exact-match boundary anyway) and a bogus method never match.
// ---------------------------------------------------------------------------

void test_route_exclude_bogus_path_never_matches(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_http_route_exclude(BB_HTTP_GET, "/api/diag/meminfo"));
    TEST_ASSERT_FALSE(route_excluded(BB_HTTP_GET, "/api/diag/meminfo-extra"));
    TEST_ASSERT_FALSE(route_excluded(BB_HTTP_POST, "/api/diag/meminfo"));
    TEST_ASSERT_FALSE(route_excluded(BB_HTTP_GET, NULL));
}

// ---------------------------------------------------------------------------
// B1-1401 HIGH fix, end-to-end mechanism-closure proof (host backend, which
// mirrors the espidf platform/handler-bound-route split via
// bb_http_register_route()'s non-/api s_host_routes[] table --
// bb_http_host.c's own header comment on bb_http_register_route()). Before
// the fix, bb_http_route_exclude(GET, "/ping") returned BB_OK, then
// bb_http_register_route() registered the real handler unconditionally
// (registry_add()/route_excluded() is never consulted for a non-/api
// handler-bound route), leaving "/ping" fully live and dispatchable --
// silent false success. This test drives exactly that sequence and asserts
// the exclusion call itself now fails loudly BEFORE any registration is
// attempted, so a caller relying on it can never reach the silent-success
// state; it then independently confirms (mirroring what a caller who
// ignored the error would still see) that the route remains reachable
// through bb_http_host_dispatch_route(), proving there genuinely is no
// choke point this primitive could have hooked for a non-/api/ path.
// ---------------------------------------------------------------------------

static bb_err_t ping_handler(bb_http_request_t *req)
{
    bb_http_resp_set_status(req, 200);
    return BB_OK;
}

void test_route_exclude_non_api_handler_bound_route_closure(void)
{
    bb_http_route_registry_clear();
    bb_http_route_exclude_reset();
    bb_http_host_reset_routes();

    // The exclusion attempt itself must fail loudly, not return BB_OK and
    // silently do nothing (the original bug).
    bb_err_t exc_err = bb_http_route_exclude(BB_HTTP_GET, "/ping");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, exc_err);

    // A handler-bound non-/api route reaches the platform backend directly
    // (bb_http_register_route(), mirroring the espidf split) with no
    // knowledge of the (rejected, never-stored) exclusion attempt above.
    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_register_route(NULL, BB_HTTP_GET, "/ping", ping_handler));

    // Reachability is unambiguous: the route dispatches and serves 200, not
    // a 404 an effective exclusion would have produced.
    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t disp_err = bb_http_host_dispatch_route(req, BB_HTTP_GET, "/ping");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, disp_err);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_reset_routes();
}
