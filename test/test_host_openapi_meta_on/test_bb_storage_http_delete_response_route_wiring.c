#include "unity.h"
#include "bb_serialize_meta_test.h"

#include "../../components/bb_diag_http/bb_storage_http_delete_wire_priv.h"

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves DELETE /api/diag/storage's 200-response runtime-compose path
// (B1-1059 emit batch C, site C5) actually wires up:
// bb_storage_http_delete_wire_ensure_schema_patched() runs exactly once and
// composes content byte-identical to bb_serialize_meta_openapi_schema()'s
// own proven output over bb_storage_http_delete_wire_desc/_meta (already
// golden-tested against the hand literal by
// test_bb_storage_http_delete_wire_meta_golden.c), and re-running it is
// pointer-stable (idempotent -- never re-composes once patched). Distinct
// compose buffer/sentinel from test_bb_storage_http_delete_route_wiring.c's
// REQUEST-schema pair (own wire.c site, own function, own buffer) -- both
// sites happen to share bb_storage_http_routes_init() as their register
// call site, but each is exercised here directly via its own wire.c
// accessor, never through init() itself.

// Full expected composed schema: same content
// test_bb_storage_http_delete_wire_meta_golden.c's k_expected_meta_schema
// proves bb_serialize_meta_openapi_schema() renders for
// bb_storage_http_delete_wire_desc/_meta -- properties/top-level "required"
// content byte-identical to the pre-existing hand literal, modulo the one
// documented structural delta (a trailing "additionalProperties":false --
// see that golden's file banner).
static const char *const k_expected_storage_delete_response_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"deleted\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
    "\"key\":{\"type\":\"string\"}},"
    "\"required\":[\"deleted\"],"
    "\"additionalProperties\":false}";

// Exercises the fail-loud `if (rc != BB_OK) return rc;` arm inside
// bb_serialize_meta_ensure_composed() as reached from
// bb_storage_http_delete_wire_ensure_schema_patched() -- forces the engine
// (bb_serialize_meta, via BB_SERIALIZE_META_TESTING's fail-injection seam)
// to return BB_ERR_NO_SPACE and asserts the compose buffer is left
// unpatched (empty string). MUST run before the two success tests below:
// the compose-and-patch step is guarded/idempotent (a non-empty buffer
// short-circuits a second real compose), so once a prior test has
// successfully composed it this seam can no longer force a re-compose --
// see test_main.c's RUN_TEST order. This is a DISTINCT compose buffer from
// every other site's own buffer (separate wire.c, separate sentinel), so
// forcing this failure never disturbs another site's state -- including
// the storage-delete REQUEST schema's own buffer.
void test_bb_storage_http_delete_response_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_storage_http_delete_wire_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_storage_http_delete_wire_get_schema());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_storage_http_delete_response_schema_matches_expected_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_http_delete_wire_ensure_schema_patched());

    const char *schema = bb_storage_http_delete_wire_get_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_storage_delete_response_schema, schema);
}

void test_bb_storage_http_delete_response_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_storage_http_delete_wire_get_schema();
    TEST_ASSERT_EQUAL_STRING(k_expected_storage_delete_response_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_storage_http_delete_wire_ensure_schema_patched();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_storage_http_delete_wire_get_schema();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_storage_delete_response_schema, second);
}
