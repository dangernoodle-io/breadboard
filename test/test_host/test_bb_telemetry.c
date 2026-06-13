// Tests for bb_telemetry section registry + get/patch dispatch.
#include "unity.h"
#include "bb_telemetry.h"
#include "bb_nv.h"

#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Simple section stubs
// ---------------------------------------------------------------------------

static int s_get_calls;
static int s_patch_calls;
static bb_err_t s_patch_rc;

static void stub_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    s_get_calls++;
    bb_json_obj_set_string(section, "key", "val");
}

static bb_err_t stub_patch(bb_json_t patch, void *ctx)
{
    (void)patch; (void)ctx;
    s_patch_calls++;
    return s_patch_rc;
}

static void reset_all(void)
{
    bb_telemetry_reset_for_test();
    bb_nv_host_str_store_reset();
    s_get_calls   = 0;
    s_patch_calls = 0;
    s_patch_rc    = BB_OK;
}

// ---------------------------------------------------------------------------
// register_section: basic / error cases
// ---------------------------------------------------------------------------

void test_bb_telemetry_register_ok(void)
{
    reset_all();
    bb_err_t rc = bb_telemetry_register_section("foo", stub_get, stub_patch, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}

void test_bb_telemetry_register_null_name_returns_invalid_arg(void)
{
    reset_all();
    bb_err_t rc = bb_telemetry_register_section(NULL, stub_get, stub_patch, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_telemetry_register_null_get_returns_invalid_arg(void)
{
    reset_all();
    bb_err_t rc = bb_telemetry_register_section("foo", NULL, stub_patch, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_telemetry_register_overflow_returns_no_space(void)
{
    reset_all();
    // Fill to capacity (default 4).
    for (int i = 0; i < CONFIG_BB_TELEMETRY_MAX_SECTIONS; i++) {
        bb_err_t rc = bb_telemetry_register_section("s", stub_get, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    }
    bb_err_t rc = bb_telemetry_register_section("over", stub_get, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
}

void test_bb_telemetry_register_readonly_null_patch_ok(void)
{
    reset_all();
    bb_err_t rc = bb_telemetry_register_section("ro", stub_get, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// build_get: empty / one / two sections
// ---------------------------------------------------------------------------

void test_bb_telemetry_build_get_empty_registry(void)
{
    reset_all();
    bb_json_t root = bb_json_obj_new();
    bb_telemetry_build_get_for_test(root);
    char *s = bb_json_serialize(root);
    TEST_ASSERT_NOT_NULL(s);
    // Empty object.
    TEST_ASSERT_EQUAL_STRING("{}", s);
    bb_json_free_str(s);
    bb_json_free(root);
}

void test_bb_telemetry_build_get_one_section(void)
{
    reset_all();
    bb_telemetry_register_section("alpha", stub_get, NULL, NULL);
    bb_json_t root = bb_json_obj_new();
    bb_telemetry_build_get_for_test(root);

    TEST_ASSERT_EQUAL_INT(1, s_get_calls);

    bb_json_t alpha = bb_json_obj_get_item(root, "alpha");
    TEST_ASSERT_NOT_NULL(alpha);

    char val[32] = {0};
    bool ok = bb_json_obj_get_string(alpha, "key", val, sizeof(val));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("val", val);

    bb_json_free(root);
}

void test_bb_telemetry_build_get_two_sections(void)
{
    reset_all();
    bb_telemetry_register_section("a", stub_get, NULL, NULL);
    bb_telemetry_register_section("b", stub_get, NULL, NULL);

    bb_json_t root = bb_json_obj_new();
    bb_telemetry_build_get_for_test(root);

    TEST_ASSERT_EQUAL_INT(2, s_get_calls);

    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "a"));
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "b"));

    bb_json_free(root);
}

// ---------------------------------------------------------------------------
// dispatch_patch: correct section / unknown ignored / read-only → err
// ---------------------------------------------------------------------------

void test_bb_telemetry_dispatch_patch_known_section(void)
{
    reset_all();
    bb_telemetry_register_section("s", stub_get, stub_patch, NULL);

    bb_json_t body = bb_json_parse("{\"s\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_telemetry_dispatch_patch_for_test(body);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, s_patch_calls);

    bb_json_free(body);
}

void test_bb_telemetry_dispatch_patch_unknown_section_ignored(void)
{
    reset_all();
    bb_telemetry_register_section("s", stub_get, stub_patch, NULL);

    bb_json_t body = bb_json_parse("{\"unknown\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_telemetry_dispatch_patch_for_test(body);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, s_patch_calls);

    bb_json_free(body);
}

void test_bb_telemetry_dispatch_patch_readonly_returns_invalid_arg(void)
{
    reset_all();
    bb_telemetry_register_section("ro", stub_get, NULL /* read-only */, NULL);

    bb_json_t body = bb_json_parse("{\"ro\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_telemetry_dispatch_patch_for_test(body);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);

    bb_json_free(body);
}
