#include "unity.h"
#include "bb_ota_check.h"
#include "bb_ota_check_internal.h"
#include "bb_data.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves POST /api/update/config's runtime-compose path (B1-1059 emit batch
// A, site 4) actually wires up: bb_ota_check_init()'s guarded
// ensure_config_post_request_schema_patched()/ensure_config_post_response_
// schema_patched() calls each run exactly once and compose content
// byte-identical to bb_serialize_meta_openapi_schema()'s own proven output
// over s_config_desc/bb_ota_check_config_meta (already golden-tested against
// the hand literal by test_bb_ota_check_config_meta_golden.c, same shape
// GET's own site 3 wiring test already proves too), and re-running each is
// pointer-stable (idempotent -- never re-composes once patched).

static const char *const k_expected_config_post_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"enabled\"],"
    "\"additionalProperties\":false}";

// Exercises the fail-loud `if (config_post_request_schema_rc != BB_OK)
// return config_post_request_schema_rc;` branch in bb_ota_check_init()
// (bb_ota_check_common.c) -- forces the engine to return BB_ERR_NO_SPACE
// and asserts init() propagates that error with the request schema left
// unpatched (NULL), rather than registering a partial/stale schema.
//
// update_available, config_get, and config_post's request/response compose
// calls all share ONE init() call site, in that fixed order -- primes
// every EARLIER compose directly (bypassing the full init(), so THIS site's
// own buffer stays untouched) so force_no_space reaches THIS site's own
// compose call specifically, rather than tripping an earlier one first (see
// test_bb_ota_check_config_get_wiring.c's own comment for the full
// mechanism). MUST run before every success test below in this file AND
// before test_bb_ota_check_config_post_response_schema_offline_on_compose_
// failure below it -- see test_main.c's RUN_TEST order.
void test_bb_ota_check_config_post_request_schema_offline_on_compose_failure(void)
{
    bb_data_test_reset();
    bb_ota_check_reset_for_test();
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_assemble_update_available_schema_for_test());
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_assemble_config_get_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_ota_check_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_ota_check_get_config_post_request_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
    bb_ota_check_reset_for_test();
}

// Same rationale as the request test above, one compose call later --
// primes update_available, config_get, AND config_post's request (this
// site's own EARLIER sibling) so force_no_space reaches THIS site's
// response compose call specifically. MUST run before every success test
// below -- see test_main.c's RUN_TEST order.
void test_bb_ota_check_config_post_response_schema_offline_on_compose_failure(void)
{
    bb_data_test_reset();
    bb_ota_check_reset_for_test();
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_assemble_update_available_schema_for_test());
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_assemble_config_get_schema_for_test());
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_assemble_config_post_request_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_ota_check_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_ota_check_get_config_post_response_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
    bb_ota_check_reset_for_test();
}

void test_bb_ota_check_config_post_request_schema_matches_expected_content(void)
{
    bb_data_test_reset();
    bb_ota_check_reset_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));

    const char *schema = bb_ota_check_get_config_post_request_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_config_post_schema, schema);
}

void test_bb_ota_check_config_post_response_schema_matches_expected_content(void)
{
    bb_data_test_reset();
    bb_ota_check_reset_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));

    const char *schema = bb_ota_check_get_config_post_response_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_config_post_schema, schema);
}

void test_bb_ota_check_config_post_request_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_ota_check_get_config_post_request_schema_for_test();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING(k_expected_config_post_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_ota_check_assemble_config_post_request_schema_for_test();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_ota_check_get_config_post_request_schema_for_test();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_config_post_schema, second);
}

void test_bb_ota_check_config_post_response_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_ota_check_get_config_post_response_schema_for_test();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING(k_expected_config_post_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_ota_check_assemble_config_post_response_schema_for_test();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_ota_check_get_config_post_response_schema_for_test();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_config_post_schema, second);
}
