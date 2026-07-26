#include "unity.h"
#include "bb_temp.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves bb_temp_register_info()'s runtime-compose path (B1-1059 PR-b)
// actually wires up. Uses the EXISTING generic bb_health_section registry
// test seam (bb_health_section_test_find()/_test_reset(), from
// BB_HEALTH_SECTION_TESTING, inherited via [env:native]'s build_flags)
// rather than a per-component test accessor: after register(), the
// registry entry's schema_props must equal the hand literal (k_temp_schema)
// -- the same content the host meta-golden test
// (test_bb_temp_health_meta_golden.c) already proves the runtime engine
// renders byte-identically.
//
// PLATFORM TWIN NOTE -- this native env links platform/host/bb_temp/
// bb_temp.c (see bbtool.toml's native board mapping), so this test only
// ever exercises the HOST twin's copy of the compose-and-patch block;
// platform/espidf/bb_temp/bb_temp.c carries a byte-identical copy (by
// convention, see that file's own banner) that is not directly reachable
// from this host test binary.

// k_temp_schema (platform/{host,espidf}/bb_temp/bb_temp.c) verbatim.
static const char *const k_hand_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"soc_c\":{\"type\":\"number\"}}}";

// The helper's own error arm (bb_serialize_meta_ensure_composed()'s
// composer-error path) is covered once, engine-side, by that helper's own
// dedicated test (B1-1204). The test below stays here (B1-1204 review fix)
// because it exercises a DIFFERENT, per-site branch: the degrade-and-
// continue arm in bb_temp_register_info() (host twin, B1-1231, mirrors
// bb_storage_http_routes_init()'s own precedent) -- forces the engine
// (bb_serialize_meta, via BB_SERIALIZE_META_TESTING's fail-injection seam)
// to return BB_ERR_NO_SPACE and asserts the section STILL registers, with
// a NULL schema_props (omitted by bb_health_schema's pointer-NULL gate)
// rather than a partial/stale schema. MUST run before the two success
// tests below: the compose-and-patch step is guarded/idempotent (the
// file-scope schema buffer's first byte != '\0' short-circuits a second
// real assemble), so once a prior test has successfully patched it this
// seam can no longer force a re-compose -- see test_main.c's RUN_TEST
// order.
void test_bb_temp_register_info_degrades_on_compose_failure(void)
{
    bb_health_section_test_reset();
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_temp_register_info();

    const bb_health_section_t *stored = bb_health_section_test_find("temp");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_NULL(stored->schema_props);

    bb_serialize_meta_openapi_test_set_force_no_space(false);
    bb_health_section_test_reset();
}

void test_bb_temp_register_info_composes_matching_schema(void)
{
    bb_health_section_test_reset();

    bb_temp_register_info();

    const bb_health_section_t *stored = bb_health_section_test_find("temp");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_NOT_NULL(stored->schema_props);
    TEST_ASSERT_EQUAL_STRING(k_hand_schema, stored->schema_props);

    bb_health_section_test_reset();
}

// Registers twice in a row (no reset between): the compose-and-patch step
// is guarded/idempotent (it only assembles once, on the first call), so a
// second bb_temp_register_info() call must not disturb the already-patched,
// still-matching content (the underlying registry itself separately
// rejects the duplicate name -- this test is only about the schema-compose
// step staying idempotent, not the registry's own dup-reject contract,
// which test_bb_health_section.c already covers).
void test_bb_temp_register_info_idempotent_same_content(void)
{
    bb_health_section_test_reset();

    bb_temp_register_info();
    bb_temp_register_info();

    const bb_health_section_t *stored = bb_health_section_test_find("temp");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_STRING(k_hand_schema, stored->schema_props);

    bb_health_section_test_reset();
}
