#include "unity.h"
#include "bb_serialize_meta_test.h"

#include "../../components/bb_diag_http/bb_diag_drops_wire_priv.h"

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves GET /api/diag/drops' runtime-compose path actually wires up:
// bb_diag_drops_wire_ensure_schema_patched() runs exactly once and composes
// content byte-identical to bb_serialize_meta_openapi_schema()'s own proven
// output over bb_diag_drops_wire_desc/_meta (already golden-tested against
// the hand literal by test_bb_diag_drops_wire_meta_golden.c), and re-running
// it is pointer-stable (idempotent -- never re-composes once patched).

static const char *const k_expected_diag_drops_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"writer_dropped\":{\"type\":\"integer\"},"
    "\"udp_dropped\":{\"type\":\"integer\"},"
    "\"event_dropped\":{\"type\":\"integer\"},"
    "\"flush_timeouts\":{\"type\":\"integer\"},"
    "\"render_fail\":{\"type\":\"integer\"},"
    "\"stale_discard\":{\"type\":\"integer\"}},"
    "\"required\":[\"writer_dropped\",\"udp_dropped\",\"event_dropped\","
    "\"flush_timeouts\",\"render_fail\",\"stale_discard\"],"
    "\"additionalProperties\":false}";

// Exercises the fail-loud `if (rc != BB_OK) return rc;` arm inside
// bb_serialize_meta_ensure_composed() as reached from
// bb_diag_drops_wire_ensure_schema_patched() -- forces the engine to return
// BB_ERR_NO_SPACE and asserts the compose buffer is left unpatched (empty
// string). MUST run before the two success tests below (see
// test_bb_diag_heap_check_wire_route_wiring.c's own ordering note -- same
// idempotent-guard rationale, a DISTINCT compose buffer from every other
// site's own buffer).
void test_bb_diag_drops_wire_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_diag_drops_wire_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_diag_drops_wire_get_schema());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_diag_drops_wire_schema_matches_expected_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_drops_wire_ensure_schema_patched());

    const char *schema = bb_diag_drops_wire_get_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_drops_schema, schema);
}

void test_bb_diag_drops_wire_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_diag_drops_wire_get_schema();
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_drops_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_diag_drops_wire_ensure_schema_patched();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_diag_drops_wire_get_schema();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_drops_schema, second);
}
