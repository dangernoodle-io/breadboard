#include "unity.h"
#include "bb_mqtt_client.h"
#include "bb_serialize_meta.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DBB_SERIALIZE_META_TESTING -- proves the
// ENGINE-level fail-injection seam (bb_serialize_meta_test.h, B1-1059 PR-b)
// itself does what every per-component wiring test
// (test_bb_mqtt_client_health_schema_wiring.c,
// test_bb_temp_health_schema_wiring.c) relies on: forcing
// BB_ERR_NO_SPACE with the same all-or-nothing overflow output a genuine
// undersized buffer produces, from BOTH composer entry points
// (bb_serialize_meta_openapi_schema() -- the pilot's
// bb_diag_storage_nvs.c's assemble_schema() shape -- and
// bb_serialize_meta_openapi_fragment() -- the mqtt/temp section-fragment
// shape). `desc`/`meta` are never dereferenced while the seam is forcing
// failure (the check runs before either composer touches them), so NULL is
// a valid, deliberate argument here.

void test_bb_serialize_meta_openapi_schema_force_no_space(void)
{
    char buf[64] = "unwritten";
    size_t len = 99;

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_serialize_meta_openapi_schema(NULL, NULL, buf, sizeof(buf), &len);
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL(0, len);
}

void test_bb_serialize_meta_openapi_fragment_force_no_space(void)
{
    char buf[64] = "unwritten";
    size_t len = 99;

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_serialize_meta_openapi_fragment(NULL, NULL, buf, sizeof(buf), &len);
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL(0, len);
}

// NULL out_len -- both force-no-space blocks guard their `*out_len = 0`
// write with `if (out_len)` (mirrors the non-force overflow path's same
// guard, already covered by test_bb_serialize_meta_openapi_overflow_null_out_len
// / _fragment_overflow_null_out_len in test/test_host/). The two tests
// above only ever pass a non-NULL out_len, so without these the force-path's
// NULL branch is a PR-introduced, never-taken arm.

void test_bb_serialize_meta_openapi_schema_force_no_space_null_out_len(void)
{
    char buf[64] = "unwritten";

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_serialize_meta_openapi_schema(NULL, NULL, buf, sizeof(buf), NULL);
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_bb_serialize_meta_openapi_fragment_force_no_space_null_out_len(void)
{
    char buf[64] = "unwritten";

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_serialize_meta_openapi_fragment(NULL, NULL, buf, sizeof(buf), NULL);
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

// force=false is the seam's default/reset state -- proven incidentally by
// every OTHER test in this test_filter dir (each composes real content with
// the seam off), so no dedicated "force=false is a no-op" test is added
// here.

// ---------------------------------------------------------------------------
// Real (non-forced) success/overflow calls, THIS env's own compiled variant
// (B1-1059 PR-b round 2) -- gcovr/Coveralls report per-file coverage built
// from the SAME source file compiled under MULTIPLE PlatformIO envs
// (native/native_lock_stats_off WITHOUT BB_SERIALIZE_META_TESTING vs
// native_openapi_runtime_meta WITH it): every branch downstream of the new
// `#ifdef BB_SERIALIZE_META_TESTING` block in bb_serialize_meta_openapi.c
// (the ctx.err != BB_OK check and its nested `if (out_len)` guards, both
// composer entry points) is a DIFFERENT compiled block-graph in this env
// than in test/test_host's (which never sets BB_SERIALIZE_META_TESTING),
// so this env must independently exercise every arm itself -- the
// registry-level wiring tests
// (test_bb_mqtt_client_health_schema_wiring.c/test_bb_temp_health_schema_wiring.c)
// only ever call bb_serialize_meta_openapi_fragment() with a correctly-sized
// buffer (force off), so this env's own compiled fragment() success path
// never sees a NULL out_len, and schema() (only ever called here via the
// force-no-space tests above, which short-circuit before touching
// ctx.err/out_len at all) is never called for real success/overflow in this
// env otherwise. Uses the same live desc/meta pair the wiring test already
// proves this env's runtime-compose path renders correctly
// (bb_mqtt_client_health_section_desc/_meta, from bb_mqtt_client.h).

void test_bb_serialize_meta_openapi_schema_overflow_too_small_cap(void)
{
    char   buf[4];
    size_t n = 12345;

    bb_err_t rc = bb_serialize_meta_openapi_schema(&bb_mqtt_client_health_section_desc,
                                                     &bb_mqtt_client_health_section_meta,
                                                     buf, sizeof(buf), &n);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_bb_serialize_meta_openapi_schema_overflow_null_out_len(void)
{
    char buf[4];

    bb_err_t rc = bb_serialize_meta_openapi_schema(&bb_mqtt_client_health_section_desc,
                                                     &bb_mqtt_client_health_section_meta,
                                                     buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_bb_serialize_meta_openapi_schema_success_non_null_out_len(void)
{
    char   buf[256];
    size_t n = 0;

    bb_err_t rc = bb_serialize_meta_openapi_schema(&bb_mqtt_client_health_section_desc,
                                                     &bb_mqtt_client_health_section_meta,
                                                     buf, sizeof(buf), &n);

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), n);
}

void test_bb_serialize_meta_openapi_schema_success_null_out_len(void)
{
    char buf[256];

    bb_err_t rc = bb_serialize_meta_openapi_schema(&bb_mqtt_client_health_section_desc,
                                                     &bb_mqtt_client_health_section_meta,
                                                     buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}

void test_bb_serialize_meta_openapi_fragment_overflow_too_small_cap(void)
{
    char   buf[4];
    size_t n = 12345;

    bb_err_t rc = bb_serialize_meta_openapi_fragment(&bb_mqtt_client_health_section_desc,
                                                       &bb_mqtt_client_health_section_meta,
                                                       buf, sizeof(buf), &n);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_bb_serialize_meta_openapi_fragment_overflow_null_out_len(void)
{
    char buf[4];

    bb_err_t rc = bb_serialize_meta_openapi_fragment(&bb_mqtt_client_health_section_desc,
                                                       &bb_mqtt_client_health_section_meta,
                                                       buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

// fragment()'s success+non-NULL-out_len arm is already exercised in this
// env by the mqtt/temp wiring tests (via ensure_*_health_schema_patched()),
// so only the NULL-out_len success arm is added here.
void test_bb_serialize_meta_openapi_fragment_success_null_out_len(void)
{
    char buf[256];

    bb_err_t rc = bb_serialize_meta_openapi_fragment(&bb_mqtt_client_health_section_desc,
                                                       &bb_mqtt_client_health_section_meta,
                                                       buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}
