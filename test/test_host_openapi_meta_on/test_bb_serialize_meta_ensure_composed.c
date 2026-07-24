#include "unity.h"
#include "bb_mqtt_client.h"
#include "bb_serialize_meta.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// engine-level coverage for bb_serialize_meta_ensure_composed() itself
// (B1-1204), the shared helper factored out of the 9 compose-at-init call
// sites: sentinel-taken (already-composed no-op), sentinel-not-taken
// (real compose), composer-error (undersized buf leaves the sentinel
// unarmed), and dispatch for BOTH composer shapes
// (bb_serialize_meta_openapi_schema()/_fragment()). Reuses the same
// real desc/meta fixture (bb_mqtt_client_health_section_desc/_meta) the
// engine-level force-no-space seam test already relies on
// (test_bb_serialize_meta_openapi_test_seam.c).

void test_bb_serialize_meta_ensure_composed_composes_when_buf_empty(void)
{
    char buf[256] = { 0 };

    bb_err_t rc = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                                      &bb_mqtt_client_health_section_desc,
                                                      &bb_mqtt_client_health_section_meta,
                                                      buf, sizeof(buf));

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_NOT_EQUAL(0, buf[0]);
}

// Undersized buf: the composer's own all-or-nothing overflow contract
// leaves buf[0] == '\0' on BB_ERR_NO_SPACE -- so a failed call leaves the
// idempotency sentinel unarmed, ready for a legitimate retry.
void test_bb_serialize_meta_ensure_composed_overflow_leaves_buf_empty(void)
{
    char buf[4] = { 0 };

    bb_err_t rc = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                                      &bb_mqtt_client_health_section_desc,
                                                      &bb_mqtt_client_health_section_meta,
                                                      buf, sizeof(buf));

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL(0, buf[0]);
}

// Proves the idempotency sentinel actually short-circuits BEFORE the
// composer is ever invoked: force the engine-level fail-injection seam to
// BB_ERR_NO_SPACE only on the SECOND call (after a real, successful first
// compose) -- if the sentinel didn't short-circuit, this second call would
// itself return BB_ERR_NO_SPACE; it instead stays BB_OK because the
// composer is never re-invoked once buf[0] != '\0'.
void test_bb_serialize_meta_ensure_composed_idempotent_second_call_is_noop(void)
{
    char buf[256] = { 0 };

    bb_err_t first = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                                         &bb_mqtt_client_health_section_desc,
                                                         &bb_mqtt_client_health_section_meta,
                                                         buf, sizeof(buf));
    TEST_ASSERT_EQUAL(BB_OK, first);
    char snapshot[256];
    strcpy(snapshot, buf);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t second = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                                          &bb_mqtt_client_health_section_desc,
                                                          &bb_mqtt_client_health_section_meta,
                                                          buf, sizeof(buf));
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, second);
    TEST_ASSERT_EQUAL_STRING(snapshot, buf);
}

// Dispatch works for bb_serialize_meta_openapi_schema() passed as composer
// -- output matches calling the composer directly.
void test_bb_serialize_meta_ensure_composed_dispatches_schema_composer(void)
{
    char buf[256] = { 0 };
    char direct[256];
    size_t n = 0;

    bb_err_t rc = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                                      &bb_mqtt_client_health_section_desc,
                                                      &bb_mqtt_client_health_section_meta,
                                                      buf, sizeof(buf));
    bb_err_t direct_rc = bb_serialize_meta_openapi_schema(&bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            direct, sizeof(direct), &n);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(BB_OK, direct_rc);
    TEST_ASSERT_EQUAL_STRING(direct, buf);
}

// Dispatch works for bb_serialize_meta_openapi_fragment() passed as
// composer -- output matches calling the composer directly.
void test_bb_serialize_meta_ensure_composed_dispatches_fragment_composer(void)
{
    char buf[256] = { 0 };
    char direct[256];
    size_t n = 0;

    bb_err_t rc = bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_fragment,
                                                      &bb_mqtt_client_health_section_desc,
                                                      &bb_mqtt_client_health_section_meta,
                                                      buf, sizeof(buf));
    bb_err_t direct_rc = bb_serialize_meta_openapi_fragment(&bb_mqtt_client_health_section_desc,
                                                              &bb_mqtt_client_health_section_meta,
                                                              direct, sizeof(direct), &n);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(BB_OK, direct_rc);
    TEST_ASSERT_EQUAL_STRING(direct, buf);
}
