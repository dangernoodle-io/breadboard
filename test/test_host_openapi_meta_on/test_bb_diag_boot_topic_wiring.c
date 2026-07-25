#include "unity.h"
#include "bb_diag_boot_wire.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves the "diag.boot" SSE topic schema's runtime-compose path (B1-1059
// SSE batch PR-3) actually wires up: bb_diag_boot_ensure_schema_patched()
// runs exactly once and composes content byte-identical to bb_serialize_
// meta_openapi_schema()'s own proven output over bb_diag_boot_wire_desc/
// _meta (already golden-tested against the hand literal by test_bb_diag_
// boot_wire_meta_golden.c), and re-running it is pointer-stable (idempotent
// -- never re-composes once patched).

// Full expected composed schema: bb_serialize_meta_openapi_topic_schema()'s
// `{"title":"DiagBoot","x-sse-topic":"diag.boot",` prefix ahead of the SAME
// body test_bb_diag_boot_wire_meta_golden.c's k_expected_meta_schema proves
// bb_serialize_meta_openapi_schema() renders for bb_diag_boot_wire_desc/
// _meta (properties/required content byte-identical to the pre-existing
// hand literal, PLUS the documented nested required/additionalProperties
// deltas -- see that golden's own banner).
static const char *const k_expected_diag_boot_schema =
    "{\"title\":\"DiagBoot\",\"x-sse-topic\":\"diag.boot\","
    "\"type\":\"object\",\"properties\":{"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"wdt_resets\":{\"type\":\"integer\"},"
    "\"panic\":{\"type\":\"object\",\"properties\":{"
    "\"available\":{\"type\":\"boolean\"},"
    "\"boots_since\":{\"type\":\"integer\"}},"
    "\"required\":[\"available\"],\"additionalProperties\":false},"
    "\"pending_verify\":{\"type\":\"boolean\"},"
    "\"rolled_back\":{\"type\":\"boolean\"},"
    "\"reboot_reason\":{\"type\":\"object\",\"properties\":{"
    "\"source\":{\"type\":\"string\"},"
    "\"detail\":{\"type\":\"string\"},"
    "\"uptime_s\":{\"type\":\"integer\"},"
    "\"epoch_s\":{\"type\":\"integer\"},"
    "\"age_s\":{\"type\":\"integer\"}},"
    "\"required\":[\"source\",\"uptime_s\",\"epoch_s\"],\"additionalProperties\":false},"
    "\"reboot_history\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"source\":{\"type\":\"string\"},"
    "\"epoch_s\":{\"type\":\"integer\"},"
    "\"uptime_s\":{\"type\":\"integer\"}},"
    "\"additionalProperties\":false}}},"
    "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\","
    "\"pending_verify\",\"rolled_back\",\"reboot_reason\",\"reboot_history\"],"
    "\"additionalProperties\":false}";

// Exercises bb_diag_boot_ensure_schema_patched()'s own failure contract
// directly (not via bb_diag_routes_init(), which is ESP_PLATFORM-gated and
// not host-testable): forces the engine (bb_serialize_meta, via
// BB_SERIALIZE_META_TESTING's fail-injection seam) to return
// BB_ERR_NO_SPACE and asserts the error propagates with the schema buffer
// left unpatched (empty), rather than composing a partial/stale schema --
// the invariant bb_diag_routes_init()'s CONFIG_BB_OPENAPI_RUNTIME_META
// guard relies on to skip registration on a compose failure. MUST run
// before the two success tests below: the compose-and-patch step is
// guarded/idempotent (a non-empty schema buffer short-circuits a second
// real compose), so once a prior test has successfully composed it this
// seam can no longer force a re-compose -- see test_main.c's RUN_TEST
// order.
void test_bb_diag_boot_topic_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_diag_boot_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_diag_boot_get_schema());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_diag_boot_topic_schema_matches_expected_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_boot_ensure_schema_patched());

    const char *schema = bb_diag_boot_get_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_boot_schema, schema);
}

void test_bb_diag_boot_topic_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_diag_boot_get_schema();
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_boot_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_diag_boot_ensure_schema_patched();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_diag_boot_get_schema();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_boot_schema, second);
}
