#include "unity.h"
#include "bb_info.h"
#include "bb_info_test.h"
#include "bb_json.h"
#include "bb_log.h"

#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#include "../../components/bb_info/bb_info_schema_priv.h"

static int s_get_call_count = 0;

static void test_section_get_fn(bb_json_t section, void *ctx)
{
    (void)ctx;
    s_get_call_count++;
    bb_json_obj_set_string(section, "test_field", "hello");
}

static void test_section_get_fn2(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_string(section, "other_field", "world");
}

// ---------------------------------------------------------------------------
// Section registration tests
// ---------------------------------------------------------------------------

// (1) null name → BB_ERR_INVALID_ARG
void test_bb_info_register_section_null_name_returns_err(void)
{
    bb_err_t err = bb_info_register_section(NULL, test_section_get_fn, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

// (2) null get → BB_ERR_INVALID_ARG
void test_bb_info_register_section_null_get_returns_err(void)
{
    bb_err_t err = bb_info_register_section("mysec", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

// (3) null schema → BB_OK (schema contribution skipped)
void test_bb_info_register_section_null_schema_succeeds(void)
{
    bb_err_t err = bb_info_register_section("mysec", test_section_get_fn, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    // Schema has no section entry but still parses.
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "schema is not valid JSON");
    cJSON_Delete(parsed);
}

// (4) register after freeze → BB_ERR_INVALID_STATE
void test_bb_info_register_section_after_freeze_returns_invalid_state(void)
{
    bb_info_freeze_for_test();
    bb_err_t err = bb_info_register_section("mysec", test_section_get_fn, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
}

// (5) registered section's schema_props appears in assembled schema
void test_bb_info_section_appears_in_assembled_schema(void)
{
    static const char schema_props[] = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}}}";
    bb_err_t err = bb_info_register_section("xtest", test_section_get_fn, NULL, schema_props);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"xtest\""),
                                 "section name not found in assembled schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, schema_props),
                                 "section schema_props not found in assembled schema");
}

// (6) no sections: assembled schema == base + suffix
void test_bb_info_no_sections_schema_is_base_plus_suffix(void)
{
    // No sections registered (reset happened in setUp).
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);

    char expected[8192];
    snprintf(expected, sizeof(expected), "%s%s",
             k_info_schema_base, k_info_schema_suffix);
    TEST_ASSERT_EQUAL_STRING(expected, schema);
}

// (7) two sections: both schema_props appear AND schema is valid JSON
void test_bb_info_two_sections_both_in_schema(void)
{
    static const char sp1[] = "{\"type\":\"object\",\"properties\":{\"aa\":{\"type\":\"string\"}}}";
    static const char sp2[] = "{\"type\":\"object\",\"properties\":{\"bb\":{\"type\":\"integer\"}}}";
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_info_register_section("sec1", test_section_get_fn,  NULL, sp1));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_info_register_section("sec2", test_section_get_fn2, NULL, sp2));

    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"sec1\""), "sec1 not in schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"sec2\""), "sec2 not in schema");

    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema with 2 sections is not valid JSON");
    cJSON_Delete(parsed);
}

// (8) assembled schema (no sections) is valid JSON
void test_bb_info_assembled_schema_is_valid_json(void)
{
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema is not valid JSON");
    cJSON_Delete(parsed);
}

// (9) section get_fn is invoked by bb_info_invoke_sections_for_test
void test_bb_info_section_get_fn_invoked(void)
{
    s_get_call_count = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_info_register_section("mysec", test_section_get_fn, NULL, NULL));

    bb_json_t root = bb_json_obj_new();
    bb_info_invoke_sections_for_test(root);
    TEST_ASSERT_EQUAL_INT(1, s_get_call_count);

    // Section child should be present as "mysec" key.
    bb_json_t child = bb_json_obj_get_item(root, "mysec");
    TEST_ASSERT_NOT_NULL_MESSAGE(child, "section child not set on root");

    bb_json_free(root);
}

// ---------------------------------------------------------------------------
// Capability registry tests (unchanged semantics; new API)
// ---------------------------------------------------------------------------

static void assert_assembled_schema_has_capabilities(void)
{
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"capabilities\""),
        "capabilities not found in assembled schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"items\""),
        "capabilities items not found in assembled schema");
}

// (C1) Register N capabilities → present in schema.
void test_bb_info_capabilities_registered_appear_in_schema(void)
{
    bb_info_register_capability("ota_pull");
    bb_info_register_capability("ntp");
    assert_assembled_schema_has_capabilities();
}

// (C2) Dedup: same name twice → schema still valid JSON.
void test_bb_info_capabilities_dedup(void)
{
    bb_info_register_capability("ota_push");
    bb_info_register_capability("ota_push");
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "schema with dup caps not valid JSON");
    cJSON_Delete(parsed);
}

// (C3) Empty set → capabilities array descriptor still present.
void test_bb_info_capabilities_empty_schema_present(void)
{
    assert_assembled_schema_has_capabilities();
}

// (C4) Over-capacity → extras dropped, no crash, schema still valid.
void test_bb_info_capabilities_over_cap_drops_extra(void)
{
    static const char *names[] = {
        "c00","c01","c02","c03","c04","c05","c06","c07",
        "c08","c09","c10","c11","c12","c13","c14","c15",
        "c16","c17","c18","c19","c20","c21","c22","c23",
        "c24","c25","c26","c27","c28","c29","c30","c31",
        "c32_overflow",
    };
    for (int i = 0; i <= BB_INFO_MAX_CAPABILITIES; i++) {
        bb_info_register_capability(names[i]);
    }
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "schema after cap overflow not valid JSON");
    cJSON_Delete(parsed);
}

// (C5) Post-freeze register: ignored, no crash.
void test_bb_info_capabilities_post_freeze_ignored(void)
{
    bb_info_register_capability("pre_freeze");
    bb_info_freeze_for_test();
    bb_info_register_capability("post_freeze");
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
}

// (C6) Assembled schema contains capabilities array schema descriptor.
void test_bb_info_assembled_schema_contains_capabilities_array(void)
{
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(schema, "\"capabilities\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"),
        "capabilities array schema descriptor not found in assembled schema");
}
