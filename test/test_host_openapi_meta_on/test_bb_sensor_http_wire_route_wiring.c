#include "unity.h"
#include "../../../components/bb_sensor_http/bb_sensor_http_wire_priv.h"
#include "bb_sensor_http_wire_test.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves bb_sensor_http_wire's runtime-compose path (B1-1059 PR-3 batch 3)
// actually wires up, for all four routes (GET/PATCH /api/sensors/fan, GET
// /api/sensors/power, GET /api/sensors/thermal): each guarded
// assemble-and-patch step runs exactly once and composes content
// byte-identical to the hand-authored literal (reusing PR-2's proven
// engine==literal golden fact, test_bb_sensor_http_wire_meta_golden.c), and
// re-running it is pointer-stable (idempotent -- never re-assembles once
// patched). The initial-NULL-state fact lives once in
// bb_serialize_meta_ensure_composed()'s own test (B1-1204); the
// compose-failure tests below stay here (B1-1204 review fix precedent)
// because each exercises THIS site's own `if (rc != BB_OK) return rc;`
// propagation arm in its own ensure_*_schema_patched() (bb_sensor_http_wire.c)
// -- a distinct branch from the helper's own error arm, which only the
// helper's dedicated test covers.
//
// Ordering: each site's own force-fail test MUST run before that site's own
// success test (see test_main.c's RUN_TEST order) -- the compose-and-patch
// step is guarded/idempotent (a non-empty schema buffer short-circuits a
// second real assemble), so once a prior test has successfully patched a
// given site, this seam can no longer force a re-compose for that site.

// ---------------------------------------------------------------------------
// GET /api/sensors/fan (response schema)
// ---------------------------------------------------------------------------

void test_bb_sensor_http_wire_assemble_fan_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_sensor_http_wire_assemble_fan_schema_for_test();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_sensor_http_wire_get_fan_describe_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_sensor_http_wire_assemble_fan_schema_patches_matching_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_fan_schema_for_test());

    const char *schema = bb_sensor_http_wire_get_fan_describe_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_fan_schema, schema);
}

void test_bb_sensor_http_wire_assemble_fan_schema_idempotent_pointer_stable(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_fan_schema_for_test());
    const char *first = bb_sensor_http_wire_get_fan_describe_schema_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_fan_schema_for_test());
    const char *second = bb_sensor_http_wire_get_fan_describe_schema_for_test();

    TEST_ASSERT_EQUAL_PTR(first, second);
}

// ---------------------------------------------------------------------------
// PATCH /api/sensors/fan (request schema)
// ---------------------------------------------------------------------------

void test_bb_sensor_http_wire_assemble_fan_request_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_sensor_http_wire_assemble_fan_request_schema_for_test();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_sensor_http_wire_get_fan_describe_request_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_sensor_http_wire_assemble_fan_request_schema_patches_matching_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_fan_request_schema_for_test());

    const char *schema = bb_sensor_http_wire_get_fan_describe_request_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_fan_request_schema, schema);
}

void test_bb_sensor_http_wire_assemble_fan_request_schema_idempotent_pointer_stable(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_fan_request_schema_for_test());
    const char *first = bb_sensor_http_wire_get_fan_describe_request_schema_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_fan_request_schema_for_test());
    const char *second = bb_sensor_http_wire_get_fan_describe_request_schema_for_test();

    TEST_ASSERT_EQUAL_PTR(first, second);
}

// ---------------------------------------------------------------------------
// GET /api/sensors/power (response schema)
// ---------------------------------------------------------------------------

void test_bb_sensor_http_wire_assemble_power_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_sensor_http_wire_assemble_power_schema_for_test();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_sensor_http_wire_get_power_describe_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_sensor_http_wire_assemble_power_schema_patches_matching_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_power_schema_for_test());

    const char *schema = bb_sensor_http_wire_get_power_describe_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_power_schema, schema);
}

void test_bb_sensor_http_wire_assemble_power_schema_idempotent_pointer_stable(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_power_schema_for_test());
    const char *first = bb_sensor_http_wire_get_power_describe_schema_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_power_schema_for_test());
    const char *second = bb_sensor_http_wire_get_power_describe_schema_for_test();

    TEST_ASSERT_EQUAL_PTR(first, second);
}

// ---------------------------------------------------------------------------
// GET /api/sensors/thermal (response schema)
// ---------------------------------------------------------------------------

void test_bb_sensor_http_wire_assemble_thermal_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_sensor_http_wire_assemble_thermal_schema_for_test();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(bb_sensor_http_wire_get_thermal_describe_schema_for_test());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_sensor_http_wire_assemble_thermal_schema_patches_matching_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_thermal_schema_for_test());

    const char *schema = bb_sensor_http_wire_get_thermal_describe_schema_for_test();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_thermal_schema, schema);
}

void test_bb_sensor_http_wire_assemble_thermal_schema_idempotent_pointer_stable(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_thermal_schema_for_test());
    const char *first = bb_sensor_http_wire_get_thermal_describe_schema_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bb_sensor_http_wire_assemble_thermal_schema_for_test());
    const char *second = bb_sensor_http_wire_get_thermal_describe_schema_for_test();

    TEST_ASSERT_EQUAL_PTR(first, second);
}
