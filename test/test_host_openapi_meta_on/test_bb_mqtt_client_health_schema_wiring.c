#include "unity.h"
#include "bb_mqtt_client.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves bb_mqtt_client_health_register()'s runtime-compose path (B1-1059
// PR-b) actually wires up. Uses the EXISTING generic bb_health_section
// registry test seam (bb_health_section_test_find()/_test_reset(), from
// BB_HEALTH_SECTION_TESTING, inherited via [env:native]'s build_flags)
// rather than a per-component test accessor: after register(), the
// registry entry's schema_props must equal the hand literal (k_mqtt_schema,
// bb_mqtt_client_health_section.c) -- the same content the host meta-golden
// test (test_bb_mqtt_client_health_section_meta_golden.c) already proves
// the runtime engine renders byte-identically.

// k_mqtt_schema (components/bb_mqtt_client/src/bb_mqtt_client_health_section.c)
// verbatim.
static const char *const k_hand_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"enabled\":{\"type\":\"boolean\"},"
    "\"connected\":{\"type\":\"boolean\"}}}";

// Exercises the fail-fast `if (ensure_mqtt_health_schema_patched() !=
// BB_OK) { bb_log_e(...); return; }` branch in
// bb_mqtt_client_health_register() -- forces the engine (bb_serialize_meta,
// via BB_SERIALIZE_META_TESTING's fail-injection seam) to return
// BB_ERR_NO_SPACE and asserts the section is left unregistered (offline)
// rather than registered with a partial/stale schema. MUST run before the
// two success tests below: the compose-and-patch step is guarded/idempotent
// (the file-scope schema buffer's first byte != '\0' short-circuits a
// second real assemble), so once a prior test has successfully patched it
// this seam can no longer force a re-compose -- see test_main.c's RUN_TEST
// order.
void test_bb_mqtt_client_health_register_offline_on_compose_failure(void)
{
    bb_health_section_test_reset();
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_mqtt_client_health_register();

    TEST_ASSERT_NULL(bb_health_section_test_find("mqtt"));

    bb_serialize_meta_openapi_test_set_force_no_space(false);
    bb_health_section_test_reset();
}

void test_bb_mqtt_client_health_register_composes_matching_schema(void)
{
    bb_health_section_test_reset();

    bb_mqtt_client_health_register();

    const bb_health_section_t *stored = bb_health_section_test_find("mqtt");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_NOT_NULL(stored->schema_props);
    TEST_ASSERT_EQUAL_STRING(k_hand_schema, stored->schema_props);

    bb_health_section_test_reset();
}

// Registers twice in a row (no reset between): the compose-and-patch step
// is guarded/idempotent (it only assembles once, on the first call), so a
// second bb_mqtt_client_health_register() call must not disturb the
// already-patched, still-matching content (the underlying registry itself
// separately rejects the duplicate name -- this test is only about the
// schema-compose step staying idempotent, not the registry's own
// dup-reject contract, which test_bb_health_section.c already covers).
void test_bb_mqtt_client_health_register_idempotent_same_content(void)
{
    bb_health_section_test_reset();

    bb_mqtt_client_health_register();
    bb_mqtt_client_health_register();

    const bb_health_section_t *stored = bb_health_section_test_find("mqtt");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_STRING(k_hand_schema, stored->schema_props);

    bb_health_section_test_reset();
}
