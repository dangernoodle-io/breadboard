#include "unity.h"
#include "bb_data.h"
#include "bb_http_server.h"
#include "bb_storage_http.h"
#include "bb_serialize_meta_test.h"
#include "bb_openapi.h"
#include "bb_http_host.h"

#include "../../components/bb_diag_http/bb_storage_http_delete_wire_priv.h"

#include <cJSON.h>
#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves bb_storage_http_routes.c's runtime-compose path (B1-1059 emit
// batch A, site 1) actually wires up for DELETE /api/diag/storage's
// request schema: the guarded assemble-and-patch step runs exactly once
// and composes content byte-identical to
// test_bb_storage_http_delete_apply_meta_golden.c's proven engine output
// over s_storage_delete_desc/bb_storage_http_delete_apply_meta (including
// the genuine "namespace" oneOf -- two branches, never two separate
// "namespace" properties), and re-running it is pointer-stable (idempotent
// -- never re-composes once patched). The compose-failure test below
// exercises THIS site's own `if (schema_rc != BB_OK) return schema_rc;`
// propagation arm inside ensure_storage_delete_request_schema_patched()
// (bb_storage_http_routes.c) -- a distinct branch from bb_serialize_meta_
// ensure_composed()'s own error arm (covered by its dedicated test,
// B1-1204).
//
// Ordering: the force-fail test MUST run before the success test (see
// test_main.c's RUN_TEST order) -- the compose-and-patch step is
// guarded/idempotent (a non-empty schema buffer short-circuits a second
// real assemble), so once a prior test has successfully patched it this
// seam can no longer force a re-compose.

// Composed-body target: reproduces test_bb_storage_http_delete_apply_meta_
// golden.c's k_expected_meta_schema exactly (same production desc/meta
// pair) -- "properties"/"required" key order follows physical field
// declaration order (confirm, wipe_wifi, backend, key, namespace), not the
// hand literal's authoring order, plus a trailing
// "additionalProperties":false the hand literal never had. See that
// golden's own file banner for the full accepted-delta detail.
static const char *const k_expected_storage_delete_request_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"confirm\":{\"type\":\"boolean\"},"
    "\"wipe_wifi\":{\"type\":\"boolean\"},"
    "\"backend\":{\"type\":\"string\"},"
    "\"key\":{\"type\":\"string\"},"
    "\"namespace\":{\"oneOf\":[{\"type\":\"string\"},"
    "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}]}},"
    "\"required\":[\"confirm\",\"namespace\"],"
    "\"additionalProperties\":false}";

// B1-1231: exercises bb_storage_http_routes_init()'s own degrade-and-continue
// arms directly (not just the assemble accessors above) -- proves a compose
// failure no longer aborts real bring-up: the route still registers and
// BB_OK is still returned, with BOTH the request schema (buffer-based,
// s_storage_delete_route.request_schema) and the 200 response schema
// (s_storage_delete_responses[0].schema, starts NULL) left unpatched. One
// global fail-injection flag forces both compose sites in this init fn to
// fail together -- see bb_storage_http_routes.c's own doc comment on each
// site for why each degrades safely. Runs the REAL init fn with a dummy
// handle (host bb_http_register_route() ignores the handle value, same
// posture as test_route_schema_literals_live.c's seed_live_routes()). MUST
// run before the offline-on-compose-failure test below and the success test
// further down: it also drives the real compose-and-patch steps, which are
// idempotent once they succeed -- so it must never run after a successful
// compose has already happened (see test_main.c's RUN_TEST order).
void test_bb_storage_http_routes_init_degrades_on_compose_failure(void)
{
    bb_http_route_registry_clear();
    bb_data_test_reset();

    static int s_dummy_server_target;
    bb_http_handle_t dummy = &s_dummy_server_target;

    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_storage_http_routes_init(dummy);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_NULL(bb_storage_http_delete_get_request_schema_for_test());
    TEST_ASSERT_EQUAL_STRING("", bb_storage_http_delete_wire_get_schema());
    TEST_ASSERT_EQUAL(1, bb_http_route_registry_count());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_storage_http_delete_assemble_request_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_storage_http_delete_assemble_request_schema_for_test();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_storage_http_delete_get_request_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

// B1-1244 HIGH-fix regression: proves what bb_openapi_emit_stream() actually
// EMITS for the real production DELETE /api/diag/storage route after a
// compose failure -- not just that the test accessor/struct field reads
// NULL (the offline_on_compose_failure test above, and the routes_init
// degrade test further up, already cover that), but that
// bb_openapi_emit.c's `if (route->request_schema)` gate genuinely omits
// requestBody, rather than emitting a schema-less `"application/json": {}`
// (the pre-fix bug: s_storage_delete_route.request_schema pointed DIRECTLY
// at s_storage_delete_request_schema_buf, a link-time-constant address
// that's always non-NULL even when that buffer is left empty by a failed
// compose). Drives the REAL production init fn
// (bb_storage_http_routes_init(), not a hand-built twin) with a dummy
// handle, so this exercises production's own s_storage_delete_route, not a
// copy. MUST run before the success test right below (compose is
// guarded/idempotent -- a non-empty buffer short-circuits a second real
// assemble once one has succeeded).
void test_bb_storage_http_delete_route_omits_request_body_on_compose_failure(void)
{
    bb_http_route_registry_clear();
    bb_data_test_reset();

    static int s_dummy_server_target;
    bb_http_handle_t dummy = &s_dummy_server_target;

    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_storage_http_routes_init(dummy);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_NULL(bb_storage_http_delete_get_request_schema_for_test());

    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    bb_err_t emit_rc = bb_openapi_emit_stream(req, &meta);
    bb_http_host_capture_t cap = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_http_host_capture_end(req, &cap));
    TEST_ASSERT_EQUAL(BB_OK, emit_rc);
    TEST_ASSERT_NOT_NULL(cap.body);

    cJSON *doc = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL(doc);

    cJSON *storage_path = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(doc, "paths"), "/api/diag/storage");
    TEST_ASSERT_NOT_NULL(storage_path);
    cJSON *storage_delete = cJSON_GetObjectItemCaseSensitive(storage_path, "delete");
    TEST_ASSERT_NOT_NULL(storage_delete);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(storage_delete, "requestBody"));

    cJSON_Delete(doc);
    bb_http_host_capture_free(&cap);
}

void test_bb_storage_http_delete_assemble_request_schema_patches_matching_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_http_delete_assemble_request_schema_for_test());

    const char *schema = bb_storage_http_delete_get_request_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_storage_delete_request_schema, schema);
}

void test_bb_storage_http_delete_assemble_request_schema_idempotent_pointer_stable(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_http_delete_assemble_request_schema_for_test());
    const char *first = bb_storage_http_delete_get_request_schema_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_http_delete_assemble_request_schema_for_test());
    const char *second = bb_storage_http_delete_get_request_schema_for_test();

    TEST_ASSERT_EQUAL_PTR(first, second);
}
