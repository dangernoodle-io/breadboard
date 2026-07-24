#include "unity.h"
#include "bb_ota_check.h"
#include "bb_ota_check_internal.h"
#include "bb_data.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves the "update.available" SSE topic schema's runtime-compose path
// (B1-1059 SSE batch PR-2) actually wires up: bb_ota_check_init()'s guarded
// ensure_update_available_schema_patched() call runs exactly once and
// composes content byte-identical to bb_serialize_meta_openapi_topic_
// schema()'s own proven output over bb_ota_check_wire_desc/_meta (already
// golden-tested against the hand literal by test_bb_ota_check_wire_meta_
// golden.c), and re-running it is pointer-stable (idempotent -- never
// re-composes once patched). The composed body picks up a top-level
// "additionalProperties":false the pre-existing hand literal never had (the
// meta engine always closes every rendered object) -- a genuine config-ON
// tightening, expected and asserted below.

// Full expected composed schema: bb_serialize_meta_openapi_topic_schema()'s
// `{"title":"UpdateAvailable","x-sse-topic":"update.available",` prefix
// ahead of the SAME body test_bb_ota_check_wire_meta_golden.c's
// k_expected_meta_schema proves bb_serialize_meta_openapi_schema() renders
// for bb_ota_check_wire_desc/_meta (properties/required content byte-
// identical to the pre-existing hand literal, PLUS the trailing
// "additionalProperties":false delta).
static const char *const k_expected_update_available_schema =
    "{\"title\":\"UpdateAvailable\",\"x-sse-topic\":\"update.available\","
    "\"type\":\"object\",\"properties\":{"
    "\"current\":{\"type\":\"string\"},"
    "\"latest\":{\"type\":\"string\"},"
    "\"download_url\":{\"type\":\"string\"},"
    "\"available\":{\"type\":\"boolean\"},"
    "\"ts\":{\"type\":\"integer\"},"
    "\"last_check_ok\":{\"type\":\"boolean\"},"
    "\"enabled\":{\"type\":\"boolean\"},"
    "\"outcome\":{\"type\":\"string\"},"
    "\"last_check_ts\":{\"type\":\"integer\"}},"
    "\"required\":[\"current\",\"latest\",\"download_url\",\"available\","
    "\"ts\",\"last_check_ok\",\"enabled\",\"outcome\"],"
    "\"additionalProperties\":false}";

// Exercises the fail-loud `if (schema_rc != BB_OK) return schema_rc;`
// branch in bb_ota_check_init() (bb_ota_check_common.c) -- forces the
// engine (bb_serialize_meta, via BB_SERIALIZE_META_TESTING's fail-injection
// seam) to return BB_ERR_NO_SPACE and asserts init() propagates that error
// with the topic schema buffer left unpatched (empty), rather than
// registering a partial/stale schema. Distinct from bb_serialize_meta_
// ensure_topic_schema()'s own dedicated test (test_bb_serialize_meta_
// ensure_topic_schema.c) -- that covers the helper's own error arm; this
// covers THIS site's propagation arm. MUST run before the two success
// tests below: the compose-and-patch step is guarded/idempotent (a
// non-empty schema buffer short-circuits a second real compose), so once a
// prior test has successfully composed it this seam can no longer force a
// re-compose -- see test_main.c's RUN_TEST order.
void test_bb_ota_check_update_available_schema_offline_on_compose_failure(void)
{
    bb_data_test_reset();
    bb_ota_check_reset_for_test();
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_ota_check_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_ota_check_get_update_available_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
    bb_ota_check_reset_for_test();
}

void test_bb_ota_check_update_available_schema_matches_expected_content(void)
{
    bb_data_test_reset();
    bb_ota_check_reset_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));

    const char *schema = bb_ota_check_get_update_available_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_update_available_schema, schema);
}

// Re-runs the compose-and-patch step directly (bypassing bb_ota_check_
// init()'s own s_initialized guard) with the fail-injection seam armed --
// if the buf[0] sentinel didn't short-circuit, this would itself return
// BB_ERR_NO_SPACE and clobber the buffer. Proves both pointer stability and
// content stability across a second call.
void test_bb_ota_check_update_available_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_ota_check_get_update_available_schema_for_test();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING(k_expected_update_available_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_ota_check_assemble_update_available_schema_for_test();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_ota_check_get_update_available_schema_for_test();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_update_available_schema, second);
}
