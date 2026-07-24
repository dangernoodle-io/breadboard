#include "unity.h"
#include "bb_storage_http.h"
#include "bb_serialize_meta_test.h"

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

void test_bb_storage_http_delete_assemble_request_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_storage_http_delete_assemble_request_schema_for_test();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_storage_http_delete_get_request_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
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
