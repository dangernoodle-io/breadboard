#include "unity.h"
#include "bb_diag_storage_nvs.h"
#include "bb_diag_storage_nvs_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves bb_diag_storage_nvs's runtime-compose path (B1-1059 PR-2 pilot)
// actually wires up: the describe route's 200 response starts unpatched
// (NULL schema), the guarded assemble-and-patch runs exactly once and
// composes content byte-identical to the hand-authored literal (reusing
// PR-1's proven engine==literal golden fact), and re-running it is
// pointer-stable (idempotent -- never re-assembles once patched).

void test_bb_diag_storage_nvs_describe_schema_starts_null(void)
{
    TEST_ASSERT_NULL(bb_diag_storage_nvs_get_describe_schema_for_test());
}

void test_bb_diag_storage_nvs_assemble_schema_patches_matching_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_assemble_schema_for_test());

    const char *schema = bb_diag_storage_nvs_get_describe_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(bb_diag_storage_nvs_schema, schema);
}

void test_bb_diag_storage_nvs_assemble_schema_idempotent_pointer_stable(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_assemble_schema_for_test());
    const char *first = bb_diag_storage_nvs_get_describe_schema_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_assemble_schema_for_test());
    const char *second = bb_diag_storage_nvs_get_describe_schema_for_test();

    TEST_ASSERT_EQUAL_PTR(first, second);
}
