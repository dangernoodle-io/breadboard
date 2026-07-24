#include "unity.h"
#include "bb_serialize_meta_test.h"

#include "../../components/bb_diag_http/bb_diag_tasks_get_wire_priv.h"

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves GET /api/diag/tasks's runtime-compose path (B1-1059 emit batch C,
// site C3) actually wires up: bb_diag_tasks_get_wire_ensure_schema_patched()
// runs exactly once and composes content byte-identical to
// bb_serialize_meta_openapi_schema()'s own proven output over
// bb_diag_tasks_get_wire_desc/_meta (already golden-tested against the hand
// literal by test_bb_diag_tasks_get_wire_meta_golden.c), and re-running it
// is pointer-stable (idempotent -- never re-composes once patched).

// Full expected composed schema: same content
// test_bb_diag_tasks_get_wire_meta_golden.c's k_expected_meta_schema proves
// bb_serialize_meta_openapi_schema() renders for
// bb_diag_tasks_get_wire_desc/_meta -- properties/top-level "required"
// content byte-identical to the pre-existing (UNCHANGED) hand literal,
// modulo the three documented structural deltas (a trailing
// "additionalProperties":false at every object depth; the "tasks"
// ARR-of-OBJ items object never gets a "required" list -- see that
// golden's file banner).
static const char *const k_expected_diag_tasks_get_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"tasks\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\"},"
    "\"prio\":{\"type\":\"integer\"},"
    "\"base_prio\":{\"type\":\"integer\"},"
    "\"stack_hwm\":{\"type\":\"integer\"},"
    "\"state\":{\"type\":\"string\"},"
    "\"core\":{\"type\":\"integer\"},"
    "\"runtime\":{\"type\":\"integer\"},"
    "\"stack_budget_bytes\":{\"type\":\"integer\"},"
    "\"wdt_subscribed\":{\"type\":\"boolean\"},"
    "\"sw_wdt_timeout_ms\":{\"type\":\"integer\"},"
    "\"sw_wdt_last_feed_age_ms\":{\"type\":\"integer\"},"
    "\"sw_wdt_miss_count\":{\"type\":\"integer\"},"
    "\"sw_wdt_last_miss_age_ms\":{\"type\":\"integer\"}},"
    "\"additionalProperties\":false}},"
    "\"registry\":{\"type\":\"object\",\"properties\":{"
    "\"count\":{\"type\":\"integer\"},"
    "\"capacity\":{\"type\":\"integer\"},"
    "\"dropped\":{\"type\":\"integer\"}},"
    "\"required\":[\"count\",\"capacity\",\"dropped\"],"
    "\"additionalProperties\":false}},"
    "\"required\":[\"tasks\",\"registry\"],"
    "\"additionalProperties\":false}";

// Exercises the fail-loud `if (rc != BB_OK) return rc;` arm inside
// bb_serialize_meta_ensure_composed() as reached from
// bb_diag_tasks_get_wire_ensure_schema_patched() -- forces the engine
// (bb_serialize_meta, via BB_SERIALIZE_META_TESTING's fail-injection seam)
// to return BB_ERR_NO_SPACE and asserts the compose buffer is left
// unpatched (empty string). MUST run before the two success tests below:
// the compose-and-patch step is guarded/idempotent (a non-empty buffer
// short-circuits a second real compose), so once a prior test has
// successfully composed it this seam can no longer force a re-compose --
// see test_main.c's RUN_TEST order. This is a DISTINCT compose buffer from
// every other site's own buffer (separate wire.c, separate sentinel), so
// forcing this failure never disturbs another site's state.
void test_bb_diag_tasks_get_wire_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_diag_tasks_get_wire_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_diag_tasks_get_wire_get_schema());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_diag_tasks_get_wire_schema_matches_expected_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_tasks_get_wire_ensure_schema_patched());

    const char *schema = bb_diag_tasks_get_wire_get_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_tasks_get_schema, schema);
}

void test_bb_diag_tasks_get_wire_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_diag_tasks_get_wire_get_schema();
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_tasks_get_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_diag_tasks_get_wire_ensure_schema_patched();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_diag_tasks_get_wire_get_schema();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_tasks_get_schema, second);
}
