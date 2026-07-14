// Tests for bb_response named-section registry.
#include "unity.h"
#include "bb_response.h"
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

static bb_response_registry_t make_reg(void)
{
    bb_response_registry_t r;
    memset(&r, 0, sizeof(r));
    r.tag = "test";
    return r;
}

// ---------------------------------------------------------------------------
// bb_response_register: basic / error cases
// ---------------------------------------------------------------------------

void test_bb_response_register_ok(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_err_t rc = bb_response_register(&reg, "foo", stub_get, stub_patch, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, reg.count);
}

void test_bb_response_register_null_reg_returns_invalid_arg(void)
{
    reset_stub();
    bb_err_t rc = bb_response_register(NULL, "foo", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_response_register_null_name_returns_invalid_arg(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_err_t rc = bb_response_register(&reg, NULL, stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_response_register_null_get_returns_invalid_arg(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_err_t rc = bb_response_register(&reg, "foo", NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_response_register_readonly_null_patch_ok(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_err_t rc = bb_response_register(&reg, "ro", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}

void test_bb_response_register_capacity_returns_no_space(void)
{
    static const char *k_names[] = { "s0","s1","s2","s3","s4","s5","s6","s7" };
    bb_response_registry_t reg = make_reg();
    reset_stub();
    for (int i = 0; i < BB_RESPONSE_MAX; i++) {
        bb_err_t rc = bb_response_register(&reg, k_names[i], stub_get, NULL, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    }
    bb_err_t rc = bb_response_register(&reg, "over", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// bb_response_build_get: named children appear in root
// ---------------------------------------------------------------------------

void test_bb_response_build_get_empty_registry(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_json_t root = bb_json_obj_new();
    bb_response_build_get(&reg, root);
    char *s = bb_json_serialize(root);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("{}", s);
    bb_json_free_str(s);
    bb_json_free(root);
}

void test_bb_response_build_get_one_section(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_response_register(&reg, "alpha", stub_get, NULL, NULL, NULL);

    bb_json_t root = bb_json_obj_new();
    bb_response_build_get(&reg, root);

    TEST_ASSERT_EQUAL_INT(1, s_get_calls);
    bb_json_t alpha = bb_json_obj_get_item(root, "alpha");
    TEST_ASSERT_NOT_NULL(alpha);
    char val[32] = {0};
    bool ok = bb_json_obj_get_string(alpha, "key", val, sizeof(val));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("val", val);
    bb_json_free(root);
}

void test_bb_response_build_get_two_sections(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_response_register(&reg, "a", stub_get, NULL, NULL, NULL);
    bb_response_register(&reg, "b", stub_get, NULL, NULL, NULL);

    bb_json_t root = bb_json_obj_new();
    bb_response_build_get(&reg, root);
    TEST_ASSERT_EQUAL_INT(2, s_get_calls);
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "a"));
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "b"));
    bb_json_free(root);
}

// ---------------------------------------------------------------------------
// bb_response_dispatch_patch: patchable / read-only / unknown
// ---------------------------------------------------------------------------

void test_bb_response_dispatch_patch_known_patchable(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_response_register(&reg, "s", stub_get, stub_patch, NULL, NULL);

    bb_json_t body = bb_json_parse("{\"s\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_response_dispatch_patch(&reg, body);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, s_patch_calls);
    bb_json_free(body);
}

void test_bb_response_dispatch_patch_readonly_returns_invalid_arg(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_response_register(&reg, "ro", stub_get, NULL /* read-only */, NULL, NULL);

    bb_json_t body = bb_json_parse("{\"ro\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_response_dispatch_patch(&reg, body);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
    bb_json_free(body);
}

void test_bb_response_dispatch_patch_unknown_section_ignored(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_response_register(&reg, "s", stub_get, stub_patch, NULL, NULL);

    bb_json_t body = bb_json_parse("{\"unknown\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_response_dispatch_patch(&reg, body);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, s_patch_calls);
    bb_json_free(body);
}

// ---------------------------------------------------------------------------
// bb_response_assemble_schema: output correctness
// ---------------------------------------------------------------------------

void test_bb_response_assemble_schema_no_sections(void)
{
    bb_response_registry_t reg = make_reg();
    char *s = bb_response_assemble_schema(&reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"object\",\"properties\":{}}", s);
    free(s);
}

void test_bb_response_assemble_schema_one_section_with_props(void)
{
    bb_response_registry_t reg = make_reg();
    bb_response_register(&reg, "foo", stub_get, NULL, NULL,
                         "{\"type\":\"object\"}");

    char *s = bb_response_assemble_schema(&reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"foo\":{\"type\":\"object\"}"));
    free(s);
}

void test_bb_response_assemble_schema_two_sections(void)
{
    bb_response_registry_t reg = make_reg();
    bb_response_register(&reg, "a", stub_get, NULL, NULL, "{\"type\":\"string\"}");
    bb_response_register(&reg, "b", stub_get, NULL, NULL, "{\"type\":\"number\"}");

    char *s = bb_response_assemble_schema(&reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"a\":{\"type\":\"string\"}"));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"b\":{\"type\":\"number\"}"));
    free(s);
}

void test_bb_response_assemble_schema_null_schema_props_omitted(void)
{
    bb_response_registry_t reg = make_reg();
    // No schema_props — section should not appear in assembled schema.
    bb_response_register(&reg, "noprops", stub_get, NULL, NULL, NULL);

    char *s = bb_response_assemble_schema(&reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
    TEST_ASSERT_NOT_NULL(s);
    // Schema should be empty properties (section omitted).
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"object\",\"properties\":{}}", s);
    free(s);
}

// ---------------------------------------------------------------------------
// bb_response_freeze: post-freeze register rejected
// ---------------------------------------------------------------------------

void test_bb_response_freeze_rejects_register_after(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_response_register(&reg, "a", stub_get, NULL, NULL, NULL);
    bb_response_freeze(&reg);

    bb_err_t rc = bb_response_register(&reg, "b", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, rc);
    // First registration still accessible.
    TEST_ASSERT_EQUAL_INT(1, reg.count);
}

void test_bb_response_freeze_build_get_still_works(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_response_register(&reg, "x", stub_get, NULL, NULL, NULL);
    bb_response_freeze(&reg);

    bb_json_t root = bb_json_obj_new();
    bb_response_build_get(&reg, root);
    TEST_ASSERT_EQUAL_INT(1, s_get_calls);
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "x"));
    bb_json_free(root);
}

// ---------------------------------------------------------------------------
// Duplicate-name detection (F-04)
// ---------------------------------------------------------------------------

void test_bb_response_register_dup_name_returns_invalid_state(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_err_t rc = bb_response_register(&reg, "foo", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, reg.count);

    // Second registration with the same name must be rejected.
    rc = bb_response_register(&reg, "foo", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, rc);
    TEST_ASSERT_EQUAL_INT(1, reg.count);  // count unchanged
}

void test_bb_response_register_dup_name_different_case_allowed(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    // "Foo" and "foo" are different names — case-sensitive compare.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_response_register(&reg, "Foo", stub_get, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_response_register(&reg, "foo", stub_get, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, reg.count);
}

// ---------------------------------------------------------------------------
// Multi-section PATCH partial-apply prevention (F3)
// ---------------------------------------------------------------------------

static bb_err_t patch_fan(bb_json_t p, void *ctx) { (void)p; (void)ctx; s_patch_calls++; return BB_OK; }

void test_bb_response_dispatch_patch_multi_read_only_rejects_all(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    // "fan" is writable, "power" is read-only.
    bb_response_register(&reg, "fan",   stub_get, patch_fan, NULL, NULL);
    bb_response_register(&reg, "power", stub_get, NULL /* ro */, NULL, NULL);

    // Body targets both fan (writable) and power (read-only).
    bb_json_t body = bb_json_parse("{\"fan\":{\"duty_pct\":50},\"power\":{\"vout_mv\":1200}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_response_dispatch_patch(&reg, body);
    bb_json_free(body);

    // Must fail with INVALID_ARG (power is read-only).
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
    // Fan patch_fn must NOT have been called (validate-before-apply).
    TEST_ASSERT_EQUAL_INT(0, s_patch_calls);
}

void test_bb_response_dispatch_patch_single_writable_applies(void)
{
    bb_response_registry_t reg = make_reg();
    reset_stub();
    bb_response_register(&reg, "fan",   stub_get, patch_fan, NULL, NULL);
    bb_response_register(&reg, "power", stub_get, NULL /* ro */, NULL, NULL);

    // Body targets only fan (writable) — should succeed.
    bb_json_t body = bb_json_parse("{\"fan\":{\"duty_pct\":50}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_response_dispatch_patch(&reg, body);
    bb_json_free(body);

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, s_patch_calls);
}

// ---------------------------------------------------------------------------
// assemble_schema: MIXED null + non-null schema_props (A F-09)
// ---------------------------------------------------------------------------

void test_bb_response_assemble_schema_mixed_null_and_props(void)
{
    bb_response_registry_t reg = make_reg();
    // Register two sections: one with schema_props, one without.
    bb_response_register(&reg, "diag",      stub_get, NULL, NULL, "{\"type\":\"object\"}");
    bb_response_register(&reg, "noprops",   stub_get, NULL, NULL, NULL);
    bb_response_register(&reg, "ntp",       stub_get, NULL, NULL, "{\"type\":\"object\",\"properties\":{\"synced\":{\"type\":\"boolean\"}}}");

    char *s = bb_response_assemble_schema(&reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
    TEST_ASSERT_NOT_NULL(s);
    // "diag" and "ntp" must appear; "noprops" must not.
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s, "\"diag\""),    "diag missing from schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s, "\"ntp\""),     "ntp missing from schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(s, "\"noprops\""),     "noprops should be omitted");

    // Must be valid JSON (parse via bb_json to avoid cJSON coupling).
    bb_json_t parsed = bb_json_parse(s, strlen(s));
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "mixed-null schema is not valid JSON");
    bb_json_free(parsed);
    free(s);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// P0 regression: diag section registers AFTER info route init (B F1)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Per-instance capacity (cap field)
// ---------------------------------------------------------------------------

void test_bb_response_register_per_instance_cap(void)
{
    bb_response_registry_t reg;
    memset(&reg, 0, sizeof(reg));
    reg.tag = "test";
    reg.cap = 2;  // tighter limit than BB_RESPONSE_MAX

    reset_stub();
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_response_register(&reg, "a", stub_get, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_response_register(&reg, "b", stub_get, NULL, NULL, NULL));

    // One more entry must be refused even though BB_RESPONSE_MAX > 2.
    bb_err_t rc = bb_response_register(&reg, "c", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_INT(2, reg.count);
}

void test_bb_response_register_cap_zero_uses_default(void)
{
    // cap==0 means fall back to BB_RESPONSE_MAX (back-compat with zero-init registries).
    bb_response_registry_t reg = make_reg();  // memset to 0, cap==0
    reset_stub();

    static const char *k_names[] = { "s0","s1","s2","s3","s4","s5","s6","s7" };
    for (int i = 0; i < BB_RESPONSE_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(BB_OK,
            bb_response_register(&reg, k_names[i], stub_get, NULL, NULL, NULL));
    }
    // Full at BB_RESPONSE_MAX.
    bb_err_t rc = bb_response_register(&reg, "over", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
}

void test_bb_response_register_cap_exceeds_max_clamped(void)
{
    // cap > BB_RESPONSE_MAX must be clamped to BB_RESPONSE_MAX — OOB write prevention.
    bb_response_registry_t reg;
    memset(&reg, 0, sizeof(reg));
    reg.tag = "test";
    reg.cap = 255;  // far exceeds BB_RESPONSE_MAX

    reset_stub();
    static const char *k_names[] = { "s0","s1","s2","s3","s4","s5","s6","s7" };
    for (int i = 0; i < BB_RESPONSE_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(BB_OK,
            bb_response_register(&reg, k_names[i], stub_get, NULL, NULL, NULL));
    }
    // Must cap at BB_RESPONSE_MAX, not at 255.
    bb_err_t rc = bb_response_register(&reg, "over", stub_get, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_INT(BB_RESPONSE_MAX, reg.count);
}
