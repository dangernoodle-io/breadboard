#include "unity.h"
#include "bb_system.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Composed-body target: the engine always adds an empty "required":[] array
// (neither field is required, same as the hand literal's intent) plus a
// trailing "additionalProperties":false the hand literal never had -- the
// two accepted, documented deltas test_bb_system_reboot_meta_golden.c's
// k_expected_meta_schema already proves against s_reboot_desc/
// bb_system_reboot_meta. Comparing against the raw hand literal
// (bb_system_reboot_request_schema) here would be a false fidelity failure.
static const char *const k_expected_composed_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"ts\":{\"type\":\"integer\"},"
    "\"detail\":{\"type\":\"string\"}},"
    "\"required\":[],"
    "\"additionalProperties\":false}";

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves bb_system_routes.c's runtime-compose path (B1-1059 emit batch A,
// site 2) actually wires up for POST /api/reboot's request schema: the
// guarded assemble-and-patch step runs exactly once and composes content
// byte-identical to the hand-authored literal (reusing
// test_bb_system_reboot_meta_golden.c's proven engine==literal golden
// fact), and re-running it is pointer-stable (idempotent -- never
// re-assembles once patched). The compose-failure test below exercises
// THIS site's own `if (rc != BB_OK) return rc;` propagation arm inside
// ensure_reboot_request_schema_patched() (bb_system_routes.c) -- a
// distinct branch from bb_serialize_meta_ensure_composed()'s own error arm
// (covered by its dedicated test, B1-1204).
//
// Ordering: the force-fail test MUST run before the success test (see
// test_main.c's RUN_TEST order) -- the compose-and-patch step is
// guarded/idempotent (a non-empty schema buffer short-circuits a second
// real assemble), so once a prior test has successfully patched it this
// seam can no longer force a re-compose.

void test_bb_system_reboot_assemble_request_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_system_reboot_assemble_request_schema_for_test();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_system_reboot_get_request_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_system_reboot_assemble_request_schema_patches_matching_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_system_reboot_assemble_request_schema_for_test());

    const char *schema = bb_system_reboot_get_request_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_composed_schema, schema);
}

void test_bb_system_reboot_assemble_request_schema_idempotent_pointer_stable(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_system_reboot_assemble_request_schema_for_test());
    const char *first = bb_system_reboot_get_request_schema_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_system_reboot_assemble_request_schema_for_test());
    const char *second = bb_system_reboot_get_request_schema_for_test();

    TEST_ASSERT_EQUAL_PTR(first, second);
}
