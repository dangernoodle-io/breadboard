#include "unity.h"
#include "bb_ota_check.h"
#include "bb_ota_check_internal.h"
#include "bb_data.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves GET /api/update/config's runtime-compose path (B1-1059 emit batch
// A, site 3) actually wires up: bb_ota_check_init()'s guarded
// ensure_config_get_schema_patched() call runs exactly once and composes
// content byte-identical to bb_serialize_meta_openapi_schema()'s own proven
// output over s_config_desc/bb_ota_check_config_meta (already golden-tested
// against the hand literal by test_bb_ota_check_config_meta_golden.c), and
// re-running it is pointer-stable (idempotent -- never re-composes once
// patched). The composed body picks up a top-level
// "additionalProperties":false the pre-existing hand literal never had (the
// meta engine always closes every rendered object) -- a genuine config-ON
// tightening, expected and asserted below.

static const char *const k_expected_config_get_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"enabled\"],"
    "\"additionalProperties\":false}";

// Exercises the fail-loud `if (config_get_schema_rc != BB_OK) return
// config_get_schema_rc;` branch in bb_ota_check_init() (bb_ota_check_common.c)
// -- forces the engine (bb_serialize_meta, via BB_SERIALIZE_META_TESTING's
// fail-injection seam) to return BB_ERR_NO_SPACE and asserts init()
// propagates that error with the route left unpatched (NULL schema), rather
// than registering a partial/stale schema. MUST run before the two success
// tests below: the compose-and-patch step is guarded/idempotent (a
// non-empty schema buffer short-circuits a second real compose), so once a
// prior test has successfully composed it this seam can no longer force a
// re-compose.
//
// update_available and config_get share ONE init() call site
// (bb_ota_check_init()), and update_available's own compose call sits
// EARLIER in that function than this site's -- a bare bb_ota_check_init()
// call under force_no_space would trip update_available's guard first (if
// its buffer is still unpatched) and return before ever reaching THIS
// site's own compose call, leaving this branch uncovered. Primes
// update_available's buffer directly first (bypassing the full init(), so
// config_get's own buffer stays untouched) rather than relying on
// test_bb_ota_check_update_available_wiring.c's own success test to have
// already done so -- MUST run before that test regardless (see
// test_main.c's RUN_TEST order): once ANY full successful bb_ota_check_
// init() call has happened anywhere in this suite, it composes config_get's
// buffer too as a side effect (the same shared function), permanently
// defeating this test's own fail-injection.
void test_bb_ota_check_config_get_schema_offline_on_compose_failure(void)
{
    bb_data_test_reset();
    bb_ota_check_reset_for_test();
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_assemble_update_available_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_ota_check_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_ota_check_get_config_get_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
    bb_ota_check_reset_for_test();
}

void test_bb_ota_check_config_get_schema_matches_expected_content(void)
{
    bb_data_test_reset();
    bb_ota_check_reset_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));

    const char *schema = bb_ota_check_get_config_get_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_config_get_schema, schema);
}

// Re-runs the compose-and-patch step directly (bypassing bb_ota_check_
// init()'s own s_initialized guard) with the fail-injection seam armed --
// if the buf[0] sentinel didn't short-circuit, this would itself return
// BB_ERR_NO_SPACE and clobber the buffer. Proves both pointer stability and
// content stability across a second call.
void test_bb_ota_check_config_get_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_ota_check_get_config_get_schema_for_test();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING(k_expected_config_get_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_ota_check_assemble_config_get_schema_for_test();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_ota_check_get_config_get_schema_for_test();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_config_get_schema, second);
}
