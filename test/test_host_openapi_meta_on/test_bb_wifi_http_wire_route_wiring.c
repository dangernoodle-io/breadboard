#include "unity.h"
#include "bb_serialize_meta_test.h"

#include "../../components/bb_wifi_http/bb_wifi_http_wire_priv.h"

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves GET /api/wifi's runtime-compose path (B1-1059 emit batch B, site
// B1) actually wires up: bb_wifi_http_info_wire_ensure_schema_patched()
// runs exactly once and composes content byte-identical to
// bb_serialize_meta_openapi_schema()'s own proven output over
// bb_wifi_http_info_wire_desc/_meta (already golden-tested against the hand
// literal by test_bb_wifi_http_wire_meta_golden.c), and re-running it is
// pointer-stable (idempotent -- never re-composes once patched).

// Full expected composed schema: same content
// test_bb_wifi_http_wire_meta_golden.c's k_expected_meta_schema proves
// bb_serialize_meta_openapi_schema() renders for
// bb_wifi_http_info_wire_desc/_meta (properties/required content
// byte-identical to the pre-existing hand literal, minus its embedded
// "title" and plus the trailing "additionalProperties":false delta -- see
// that golden's file banner for the two documented deltas).
static const char *const k_expected_wifi_info_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"string\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
    "\"restart_sta_count\":{\"type\":\"integer\"},"
    "\"disconnect_rssi\":{\"type\":\"integer\"}},"
    "\"required\":[\"ssid\",\"connected\"],"
    "\"additionalProperties\":false}";

// Exercises the fail-loud `if (rc != BB_OK) return rc;` arm inside
// bb_serialize_meta_ensure_composed() as reached from bb_wifi_http_info_
// wire_ensure_schema_patched() -- forces the engine (bb_serialize_meta, via
// BB_SERIALIZE_META_TESTING's fail-injection seam) to return
// BB_ERR_NO_SPACE and asserts the compose buffer is left unpatched (empty
// string). MUST run before the two success tests below: the compose-and-
// patch step is guarded/idempotent (a non-empty buffer short-circuits a
// second real compose), so once a prior test has successfully composed it
// this seam can no longer force a re-compose -- see test_main.c's RUN_TEST
// order.
void test_bb_wifi_http_wire_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_wifi_http_info_wire_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_wifi_http_info_wire_get_schema());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_wifi_http_wire_schema_matches_expected_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_wifi_http_info_wire_ensure_schema_patched());

    const char *schema = bb_wifi_http_info_wire_get_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_wifi_info_schema, schema);
}

void test_bb_wifi_http_wire_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_wifi_http_info_wire_get_schema();
    TEST_ASSERT_EQUAL_STRING(k_expected_wifi_info_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_wifi_http_info_wire_ensure_schema_patched();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_wifi_http_info_wire_get_schema();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_wifi_info_schema, second);
}
