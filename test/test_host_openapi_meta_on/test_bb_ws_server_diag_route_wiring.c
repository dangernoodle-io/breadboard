#include "unity.h"
#include "bb_ws_server_diag.h"
#include "bb_ws_server_diag_test.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves bb_ws_server_diag's runtime-compose path (B1-1059 PR-3 batch 1)
// actually wires up: the describe route's 200 response starts unpatched
// (NULL schema), the guarded assemble-and-patch runs exactly once and
// composes content byte-identical to the hand-authored literal (reusing
// PR-1's proven engine==literal golden fact), and re-running it is
// pointer-stable (idempotent -- never re-assembles once patched).

void test_bb_ws_server_diag_describe_schema_starts_null(void)
{
    TEST_ASSERT_NULL(bb_ws_server_diag_get_describe_schema_for_test());
}

// Exercises the fail-loud `if (rc != BB_OK) return rc;` branch inside
// ensure_schema_patched() (bb_ws_server_diag.c) -- forces the engine
// (bb_serialize_meta, via BB_SERIALIZE_META_TESTING's fail-injection seam)
// to return BB_ERR_NO_SPACE and asserts the route is left unpatched (NULL
// schema) rather than patched with a partial/stale one. MUST run before
// the success test below: the compose-and-patch step is guarded/idempotent
// (a NULL `.schema` short-circuits a second real assemble), so once a
// prior test has successfully patched it this seam can no longer force a
// re-compose -- see test_main.c's RUN_TEST order.
void test_bb_ws_server_diag_assemble_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_ws_server_diag_assemble_schema_for_test();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_ws_server_diag_get_describe_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_ws_server_diag_assemble_schema_patches_matching_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_diag_assemble_schema_for_test());

    const char *schema = bb_ws_server_diag_get_describe_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(bb_ws_server_diag_schema, schema);
}

void test_bb_ws_server_diag_assemble_schema_idempotent_pointer_stable(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_diag_assemble_schema_for_test());
    const char *first = bb_ws_server_diag_get_describe_schema_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_diag_assemble_schema_for_test());
    const char *second = bb_ws_server_diag_get_describe_schema_for_test();

    TEST_ASSERT_EQUAL_PTR(first, second);
}
