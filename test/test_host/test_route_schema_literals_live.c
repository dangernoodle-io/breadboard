// Host coverage: schema-literal validation over routes registered by REAL
// production init functions, not fixtures (B1-1211).
//
// bb_http_resp_json_obj_set_raw() (components/bb_http_server/src/
// bb_http_json_obj.c) writes supplied bytes VERBATIM with zero JSON
// validation. bb_openapi's stream emitter (components/bb_openapi/src/
// bb_openapi_emit.c) calls it for every registered route's schema, so a
// malformed hand-authored schema literal in any production route table
// silently produces malformed /api/openapi.json at runtime while the
// emitter still returns BB_OK.
//
// test_route_schemas.c already walks bb_http_route_registry_foreach() and
// parses every schema/request_schema it finds, but it seeds the registry
// with copy-pasted FIXTURE literals, not the production descriptors
// themselves -- a fixture can silently drift from the literal it was
// copied from. This file closes that gap for the routes whose real
// *_routes_init() is BOTH declared for host builds AND genuinely
// host-compiled:
//
//   - bb_storage_http_routes_init()               -- DELETE /api/diag/storage
//   - bb_storage_http_factory_reset_routes_init()  -- POST /api/diag/factory-reset
//   - bb_log_register_routes_init()                -- GET/POST /api/log/level
//   - bb_system_routes_init()                      -- POST /api/reboot
//
// All four live under platform/espidf/ (bb_storage_http_routes.c,
// bb_log_http.c, bb_system_routes.c) -- portable code filed under
// platform/espidf/ but pulled into the host build via
// `# bbtool-scaffold-hint: source=...` lines in the owning component's
// CMakeLists.txt (the host scaffold's directory-convention walk,
// scripts/bbtool/boards.py, only looks under platform/host/<name>/ for the
// "native" board and has no other way to see these files). Calling the REAL
// init function registers the REAL bb_route_t/bb_route_response_t
// descriptors -- the exact ones bb_openapi_emit.c walks in production --
// into the same process-wide registry (components/bb_http_server/src/
// route_registry.c), so this test parses the literal that actually ships,
// not a maintained copy of it.
//
// A non-NULL dummy bb_http_handle_t satisfies each init fn's `if (!server)
// return BB_ERR_INVALID_ARG` guard: the host bb_http_register_route()
// (platform/host/bb_http_server/bb_http_host.c) ignores the handle value
// entirely, so any non-NULL pointer works.
//
// Routes that are NEVER host-compiled (ESP-IDF-only calls, or simply not
// scaffold-hinted onto the host build) keep their existing fixture-mirror
// coverage in test_route_schemas.c and test_route_fidelity.c; this file's
// job is added breadth over the routes that ARE reachable live, not a
// replacement for those.

#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_storage_http.h"
#include "bb_log_http.h"
#include "bb_system_routes.h"
#include "bb_data.h"
#include "test_route_schema_walk_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Walker: parse every non-NULL schema / request_schema literal via the
// shared test_route_schema_walk() helper (test_route_schema_walk_common.h;
// B1-1211 review -- see that header's doc comment).
// ---------------------------------------------------------------------------
// Seed the registry with REAL production route descriptors from every
// *_routes_init() that is genuinely host-compiled (see file header).
// ---------------------------------------------------------------------------

static void seed_live_routes(void)
{
    bb_http_route_registry_clear();
    bb_data_test_reset();

    static int s_dummy_server_target;
    bb_http_handle_t dummy = &s_dummy_server_target;

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_http_routes_init(dummy));                // DELETE /api/diag/storage
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_http_factory_reset_routes_init(dummy));  // POST /api/diag/factory-reset
    TEST_ASSERT_EQUAL(BB_OK, bb_log_register_routes_init(dummy));                // GET/POST /api/log/level
    TEST_ASSERT_EQUAL(BB_OK, bb_system_routes_init(dummy));                      // POST /api/reboot
}

// ---------------------------------------------------------------------------
// Presence-of-specific-route check: proves the routes THIS test file exists
// to cover (GET and POST /api/log/level, POST /api/reboot) are actually in
// the registry, not merely that *some* route/schema is non-zero. A `>0`
// count alone is still satisfied by the pre-existing storage routes even if
// bb_log_register_routes_init() silently falls back to its
// `#else !CONFIG_BB_LOG_ROUTES` no-op stub (which still returns BB_OK) --
// dropping the scaffold-hint or the -DCONFIG_BB_LOG_ROUTES=1 build flag would
// shrink the counts but leave them positive, so nothing would fail. Checking
// exact presence of these routes fails loudly instead, and is stable across
// unrelated future route additions (unlike an exact total count, which would
// need bumping every time).
// ---------------------------------------------------------------------------

typedef struct {
    bool saw_get_log_level;
    bool saw_post_log_level;
} log_level_presence_ctx_t;

static void check_log_level_route_present(const bb_route_t *route, void *ctx_)
{
    log_level_presence_ctx_t *ctx = (log_level_presence_ctx_t *)ctx_;
    if (!route || !route->path) return;
    if (strcmp(route->path, "/api/log/level") != 0) return;
    if (route->method == BB_HTTP_GET) ctx->saw_get_log_level = true;
    if (route->method == BB_HTTP_POST) ctx->saw_post_log_level = true;
}

typedef struct {
    bool saw_post_reboot;
} reboot_presence_ctx_t;

static void check_reboot_route_present(const bb_route_t *route, void *ctx_)
{
    reboot_presence_ctx_t *ctx = (reboot_presence_ctx_t *)ctx_;
    if (!route || !route->path) return;
    if (strcmp(route->path, "/api/reboot") != 0) return;
    if (route->method == BB_HTTP_POST) ctx->saw_post_reboot = true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Mandatory guard (B1-1211): an empty registry would make every assertion
// in test_route_schema_literals_live_all_parse vacuously true. This must
// never silently pass.
void test_route_schema_literals_live_registry_nonempty(void)
{
    seed_live_routes();

    TEST_ASSERT_GREATER_THAN_INT(0, (int)bb_http_route_registry_count());

    bb_http_route_registry_clear();
}

void test_route_schema_literals_live_all_parse(void)
{
    seed_live_routes();

    test_route_schema_walk_ctx_t ctx = { 0 };
    bb_http_route_registry_foreach(test_route_schema_walk, &ctx);

    TEST_ASSERT_GREATER_THAN_INT(0, ctx.route_count);
    TEST_ASSERT_GREATER_THAN_INT(0, ctx.schema_count);
    if (ctx.failures > 0) {
        TEST_FAIL_MESSAGE(ctx.first_failure);
    }

    log_level_presence_ctx_t presence = { 0 };
    bb_http_route_registry_foreach(check_log_level_route_present, &presence);
    TEST_ASSERT_TRUE_MESSAGE(presence.saw_get_log_level,
                              "GET /api/log/level missing from registry -- "
                              "bb_log_register_routes_init() fell back to its "
                              "no-op stub");
    TEST_ASSERT_TRUE_MESSAGE(presence.saw_post_log_level,
                              "POST /api/log/level missing from registry -- "
                              "bb_log_register_routes_init() fell back to its "
                              "no-op stub");

    reboot_presence_ctx_t reboot_presence = { 0 };
    bb_http_route_registry_foreach(check_reboot_route_present, &reboot_presence);
    TEST_ASSERT_TRUE_MESSAGE(reboot_presence.saw_post_reboot,
                              "POST /api/reboot missing from registry -- "
                              "bb_system_routes_init() did not register it");

    bb_http_route_registry_clear();
}

// ---------------------------------------------------------------------------
// Negative test: prove the walker itself actually catches a malformed
// literal. Fixture is local to this file only -- never registered against a
// production route table, never leaked into k_audit/k_desc_audit elsewhere.
// ---------------------------------------------------------------------------

void test_route_schema_literals_live_walker_catches_malformed(void)
{
    static const bb_route_response_t bad_responses[] = {
        // Deliberately broken: unbalanced brace.
        { 200, "application/json", "{\"type\":\"object\"", "deliberately bad" },
        { 0 },
    };
    static const bb_route_t bad_route = {
        .method    = BB_HTTP_GET,
        .path      = "/api/b1-1211-test-fixture-only",
        .responses = bad_responses,
    };

    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&bad_route));

    test_route_schema_walk_ctx_t ctx = { 0 };
    bb_http_route_registry_foreach(test_route_schema_walk, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.route_count);
    TEST_ASSERT_EQUAL_INT(1, ctx.schema_count);
    TEST_ASSERT_EQUAL_INT(1, ctx.failures);

    bb_http_route_registry_clear();
}
