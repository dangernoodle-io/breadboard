#include "unity.h"
#include "bb_serialize_meta_test.h"

#include "../../components/bb_diag_http/bb_diag_http_boot_wire_priv.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves GET /api/diag/boot's REST envelope runtime-compose path (B1-1059
// emit boot) actually wires up: bb_diag_http_boot_wire_ensure_schema_patched()
// runs exactly once and composes content byte-identical to
// test_bb_diag_boot_wire_envelope_meta_golden.c's k_expected_envelope_schema
// (the envelope composer wraps bb_serialize_meta_openapi_schema()'s own
// proven output over bb_diag_boot_wire_desc/_meta), and re-running it is
// pointer-stable (idempotent -- never re-composes once patched). DISTINCT
// buffer/symbols from the "diag.boot" SSE topic-schema compose site
// (components/bb_diag/bb_diag_boot_wire.c's
// bb_diag_boot_ensure_schema_patched()/s_diag_boot_schema_buf) -- see that
// site's own test_bb_diag_boot_topic_schema_* trio in test_main.c for the
// SSE-topic counterpart; forcing a failure here never disturbs it.

static const char *const k_expected_diag_boot_get_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"ts_ms\":{\"type\":\"integer\"},"
    "\"data\":{\"type\":\"object\",\"properties\":{"
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
    "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\",\"pending_verify\",\"rolled_back\","
    "\"reboot_reason\",\"reboot_history\"],"
    "\"additionalProperties\":false}},"
    "\"required\":[\"ts_ms\",\"data\"]}";

// Exercises the fail-loud `if (rc != BB_OK) return rc;` arm inside
// bb_serialize_meta_ensure_composed() as reached from
// bb_diag_http_boot_wire_ensure_schema_patched() -- forces the engine
// (bb_serialize_meta, via BB_SERIALIZE_META_TESTING's fail-injection seam)
// to return BB_ERR_NO_SPACE and asserts the compose buffer is left
// unpatched (empty string). MUST run before the two success tests below:
// the compose-and-patch step is guarded/idempotent (a non-empty buffer
// short-circuits a second real compose), so once a prior test has
// successfully composed it this seam can no longer force a re-compose --
// see test_main.c's RUN_TEST order. This is a DISTINCT compose buffer from
// every other site's own buffer (separate wire.c, separate sentinel), so
// forcing this failure never disturbs another site's state.
void test_bb_diag_http_boot_wire_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_diag_http_boot_wire_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_diag_http_boot_wire_get_schema());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

// Forces this composer's OWN "prefix too small to even hold the fixed
// {"ts_ms","data"} envelope opener" bounds-check branch (distinct from the
// engine-forced failure above, which fails INSIDE
// bb_serialize_meta_openapi_schema() -- this failure never reaches that
// call at all). MUST also run before the success tests below (same
// idempotent-sentinel constraint as the engine-forced test above).
void test_bb_diag_http_boot_wire_schema_offline_on_prefix_too_small(void)
{
    bb_diag_http_boot_wire_set_test_envelope_buf_cap(8);

    bb_err_t rc = bb_diag_http_boot_wire_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_diag_http_boot_wire_get_schema());

    bb_diag_http_boot_wire_set_test_envelope_buf_cap(0);
}

// Forces this composer's OWN "no room left for the closing suffix after a
// successful inner compose" bounds-check branch: shrinks the cap to exactly
// the full envelope's content length (enough for the inner
// bb_serialize_meta_openapi_schema() call to succeed, since that leaves it
// far more than enough headroom, but one byte short of prefix+data+suffix+
// NUL). MUST also run before the success tests below.
void test_bb_diag_http_boot_wire_schema_offline_on_suffix_too_small(void)
{
    size_t full_len = strlen(k_expected_diag_boot_get_schema);
    bb_diag_http_boot_wire_set_test_envelope_buf_cap(full_len);

    bb_err_t rc = bb_diag_http_boot_wire_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_diag_http_boot_wire_get_schema());

    bb_diag_http_boot_wire_set_test_envelope_buf_cap(0);
}

void test_bb_diag_http_boot_wire_schema_matches_expected_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_http_boot_wire_ensure_schema_patched());

    const char *schema = bb_diag_http_boot_wire_get_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_boot_get_schema, schema);
}

void test_bb_diag_http_boot_wire_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_diag_http_boot_wire_get_schema();
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_boot_get_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_diag_http_boot_wire_ensure_schema_patched();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_diag_http_boot_wire_get_schema();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_boot_get_schema, second);
}
