// Tests for bb_section named-section registry.
#include "unity.h"
#include "bb_section.h"
#include "bb_json.h"

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Test stub state
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

static void reset_stub(void)
{
    s_get_calls   = 0;
    s_patch_calls = 0;
    s_patch_rc    = BB_OK;
}

static bb_section_registry_t make_reg(void)
{
    bb_section_registry_t r;
    memset(&r, 0, sizeof(r));
    r.tag = "test";
    return r;
}

// ---------------------------------------------------------------------------
// bb_section_register: basic / error cases
// ---------------------------------------------------------------------------

void test_bb_section_register_ok(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_err_t rc = bb_section_register(&reg, "foo", stub_get, stub_patch, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, reg.count);
}

void test_bb_section_register_null_reg_returns_invalid_arg(void)
{
    reset_stub();
    bb_err_t rc = bb_section_register(NULL, "foo", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_section_register_null_name_returns_invalid_arg(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_err_t rc = bb_section_register(&reg, NULL, stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_section_register_null_get_returns_invalid_arg(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_err_t rc = bb_section_register(&reg, "foo", NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_section_register_readonly_null_patch_ok(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_err_t rc = bb_section_register(&reg, "ro", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}

void test_bb_section_register_capacity_returns_no_space(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    for (int i = 0; i < BB_SECTION_MAX; i++) {
        bb_err_t rc = bb_section_register(&reg, "s", stub_get, NULL, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    }
    bb_err_t rc = bb_section_register(&reg, "over", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// bb_section_build_get: named children appear in root
// ---------------------------------------------------------------------------

void test_bb_section_build_get_empty_registry(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_json_t root = bb_json_obj_new();
    bb_section_build_get(&reg, root);
    char *s = bb_json_serialize(root);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("{}", s);
    bb_json_free_str(s);
    bb_json_free(root);
}

void test_bb_section_build_get_one_section(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_section_register(&reg, "alpha", stub_get, NULL, NULL, NULL);

    bb_json_t root = bb_json_obj_new();
    bb_section_build_get(&reg, root);

    TEST_ASSERT_EQUAL_INT(1, s_get_calls);
    bb_json_t alpha = bb_json_obj_get_item(root, "alpha");
    TEST_ASSERT_NOT_NULL(alpha);
    char val[32] = {0};
    bool ok = bb_json_obj_get_string(alpha, "key", val, sizeof(val));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("val", val);
    bb_json_free(root);
}

void test_bb_section_build_get_two_sections(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_section_register(&reg, "a", stub_get, NULL, NULL, NULL);
    bb_section_register(&reg, "b", stub_get, NULL, NULL, NULL);

    bb_json_t root = bb_json_obj_new();
    bb_section_build_get(&reg, root);
    TEST_ASSERT_EQUAL_INT(2, s_get_calls);
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "a"));
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "b"));
    bb_json_free(root);
}

// ---------------------------------------------------------------------------
// bb_section_dispatch_patch: patchable / read-only / unknown
// ---------------------------------------------------------------------------

void test_bb_section_dispatch_patch_known_patchable(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_section_register(&reg, "s", stub_get, stub_patch, NULL, NULL);

    bb_json_t body = bb_json_parse("{\"s\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_section_dispatch_patch(&reg, body);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, s_patch_calls);
    bb_json_free(body);
}

void test_bb_section_dispatch_patch_readonly_returns_invalid_arg(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_section_register(&reg, "ro", stub_get, NULL /* read-only */, NULL, NULL);

    bb_json_t body = bb_json_parse("{\"ro\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_section_dispatch_patch(&reg, body);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
    bb_json_free(body);
}

void test_bb_section_dispatch_patch_unknown_section_ignored(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_section_register(&reg, "s", stub_get, stub_patch, NULL, NULL);

    bb_json_t body = bb_json_parse("{\"unknown\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_section_dispatch_patch(&reg, body);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, s_patch_calls);
    bb_json_free(body);
}

// ---------------------------------------------------------------------------
// bb_section_assemble_schema: output correctness
// ---------------------------------------------------------------------------

void test_bb_section_assemble_schema_no_sections(void)
{
    bb_section_registry_t reg = make_reg();
    char *s = bb_section_assemble_schema(&reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"object\",\"properties\":{}}", s);
    free(s);
}

void test_bb_section_assemble_schema_one_section_with_props(void)
{
    bb_section_registry_t reg = make_reg();
    bb_section_register(&reg, "foo", stub_get, NULL, NULL,
                         "{\"type\":\"object\"}");

    char *s = bb_section_assemble_schema(&reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"foo\":{\"type\":\"object\"}"));
    free(s);
}

void test_bb_section_assemble_schema_two_sections(void)
{
    bb_section_registry_t reg = make_reg();
    bb_section_register(&reg, "a", stub_get, NULL, NULL, "{\"type\":\"string\"}");
    bb_section_register(&reg, "b", stub_get, NULL, NULL, "{\"type\":\"number\"}");

    char *s = bb_section_assemble_schema(&reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"a\":{\"type\":\"string\"}"));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"b\":{\"type\":\"number\"}"));
    free(s);
}

void test_bb_section_assemble_schema_null_schema_props_omitted(void)
{
    bb_section_registry_t reg = make_reg();
    // No schema_props — section should not appear in assembled schema.
    bb_section_register(&reg, "noprops", stub_get, NULL, NULL, NULL);

    char *s = bb_section_assemble_schema(&reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
    TEST_ASSERT_NOT_NULL(s);
    // Schema should be empty properties (section omitted).
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"object\",\"properties\":{}}", s);
    free(s);
}

// ---------------------------------------------------------------------------
// bb_section_freeze: post-freeze register rejected
// ---------------------------------------------------------------------------

void test_bb_section_freeze_rejects_register_after(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_section_register(&reg, "a", stub_get, NULL, NULL, NULL);
    bb_section_freeze(&reg);

    bb_err_t rc = bb_section_register(&reg, "b", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, rc);
    // First registration still accessible.
    TEST_ASSERT_EQUAL_INT(1, reg.count);
}

void test_bb_section_freeze_build_get_still_works(void)
{
    bb_section_registry_t reg = make_reg();
    reset_stub();
    bb_section_register(&reg, "x", stub_get, NULL, NULL, NULL);
    bb_section_freeze(&reg);

    bb_json_t root = bb_json_obj_new();
    bb_section_build_get(&reg, root);
    TEST_ASSERT_EQUAL_INT(1, s_get_calls);
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "x"));
    bb_json_free(root);
}
