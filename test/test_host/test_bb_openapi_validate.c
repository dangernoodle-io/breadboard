// Unit tests for bb_openapi_validate — JSON Schema structural validator.
//
// Coverage:
//   - type keyword (match + mismatch, all supported types)
//   - required keyword (present + missing)
//   - properties keyword (nested type mismatch with dotted path)
//   - items keyword (array element violation with indexed path)
//   - enum keyword (match + mismatch)
//   - additionalProperties=false (extra key rejected; default allows extras)
//   - unknown keyword (validates successfully with a warning)
//   - malformed schema literal (BB_ERR_INVALID_ARG)
//   - NULL arguments
//   - production schema smoke: reboot, ota-check, log-level-post

#include "unity.h"
#include "bb_openapi.h"

#include <cJSON.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a simple cJSON string node.
static cJSON *make_str(const char *s)
{
    return cJSON_CreateString(s);
}

static cJSON *make_int(int n)
{
    return cJSON_CreateNumber((double)n);
}

static cJSON *make_bool(bool b)
{
    return b ? cJSON_CreateTrue() : cJSON_CreateFalse();
}

// ---------------------------------------------------------------------------
// NULL argument tests
// ---------------------------------------------------------------------------

void test_validate_null_schema_json_returns_invalid_arg(void)
{
    cJSON *value = cJSON_CreateObject();
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(NULL, value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
    cJSON_Delete(value);
}

void test_validate_null_value_returns_invalid_arg(void)
{
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate("{\"type\":\"object\"}", NULL, &err);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
}

// ---------------------------------------------------------------------------
// Malformed schema
// ---------------------------------------------------------------------------

void test_validate_malformed_schema_returns_invalid_arg(void)
{
    cJSON *value = cJSON_CreateObject();
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate("{not valid json", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// type keyword
// ---------------------------------------------------------------------------

void test_validate_type_string_match(void)
{
    cJSON *value = make_str("hello");
    bb_err_t rc = bb_openapi_validate("{\"type\":\"string\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_type_string_mismatch(void)
{
    cJSON *value = make_int(42);
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate("{\"type\":\"string\"}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    // path is root — should be empty
    TEST_ASSERT_EQUAL_STRING("", err.path);
    cJSON_Delete(value);
}

void test_validate_type_integer_match(void)
{
    cJSON *value = make_int(7);
    bb_err_t rc = bb_openapi_validate("{\"type\":\"integer\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_type_integer_mismatch(void)
{
    cJSON *value = make_str("not-a-number");
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate("{\"type\":\"integer\"}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

void test_validate_type_boolean_match(void)
{
    cJSON *value = make_bool(true);
    bb_err_t rc = bb_openapi_validate("{\"type\":\"boolean\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_type_boolean_mismatch(void)
{
    cJSON *value = make_str("true");
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate("{\"type\":\"boolean\"}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

void test_validate_type_object_match(void)
{
    cJSON *value = cJSON_CreateObject();
    bb_err_t rc = bb_openapi_validate("{\"type\":\"object\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_type_object_mismatch(void)
{
    cJSON *value = cJSON_CreateArray();
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate("{\"type\":\"object\"}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

void test_validate_type_array_match(void)
{
    cJSON *value = cJSON_CreateArray();
    bb_err_t rc = bb_openapi_validate("{\"type\":\"array\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_type_array_mismatch(void)
{
    cJSON *value = cJSON_CreateObject();
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate("{\"type\":\"array\"}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

void test_validate_err_null_still_returns_validation_code(void)
{
    // err=NULL should not crash on a type mismatch
    cJSON *value = make_int(1);
    bb_err_t rc = bb_openapi_validate("{\"type\":\"string\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// required keyword
// ---------------------------------------------------------------------------

void test_validate_required_present(void)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "status", "ok");
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\",\"required\":[\"status\"]}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_required_missing(void)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "other", "x");
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\",\"required\":[\"status\"]}",
        value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    // message should mention the missing key
    TEST_ASSERT_NOT_NULL(strstr(err.message, "status"));
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// properties keyword — nested type mismatch with dotted path
// ---------------------------------------------------------------------------

void test_validate_properties_nested_ok(void)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddNumberToObject(value, "count", 3);
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\","
        "\"properties\":{\"count\":{\"type\":\"integer\"}}}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_properties_nested_type_mismatch(void)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "count", "not-a-number");
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\","
        "\"properties\":{\"count\":{\"type\":\"integer\"}}}",
        value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_EQUAL_STRING("count", err.path);
    cJSON_Delete(value);
}

void test_validate_properties_deeply_nested(void)
{
    // Nested: {inner: {val: "wrong-type"}} expects val to be integer
    cJSON *inner = cJSON_CreateObject();
    cJSON_AddStringToObject(inner, "val", "bad");
    cJSON *value = cJSON_CreateObject();
    cJSON_AddItemToObject(value, "inner", inner);

    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"inner\":{\"type\":\"object\","
        "\"properties\":{\"val\":{\"type\":\"integer\"}}}}}",
        value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_EQUAL_STRING("inner.val", err.path);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// items keyword — array element violation with indexed path
// ---------------------------------------------------------------------------

void test_validate_items_all_ok(void)
{
    cJSON *value = cJSON_CreateArray();
    cJSON_AddItemToArray(value, make_str("a"));
    cJSON_AddItemToArray(value, make_str("b"));
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_items_element_mismatch(void)
{
    cJSON *value = cJSON_CreateArray();
    cJSON_AddItemToArray(value, make_str("ok"));
    cJSON_AddItemToArray(value, make_int(99));  // violates string type
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}",
        value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_EQUAL_STRING("[1]", err.path);
    cJSON_Delete(value);
}

void test_validate_items_nested_object_element(void)
{
    // Array of objects; second element is missing required 'x'
    cJSON *elem0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(elem0, "x", 1);
    cJSON *elem1 = cJSON_CreateObject();  // missing 'x'
    cJSON_AddStringToObject(elem1, "other", "y");

    cJSON *value = cJSON_CreateArray();
    cJSON_AddItemToArray(value, elem0);
    cJSON_AddItemToArray(value, elem1);

    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"array\","
        "\"items\":{\"type\":\"object\",\"required\":[\"x\"]}}",
        value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_EQUAL_STRING("[1]", err.path);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// enum keyword
// ---------------------------------------------------------------------------

void test_validate_enum_match(void)
{
    cJSON *value = make_str("info");
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"string\","
        "\"enum\":[\"none\",\"error\",\"warn\",\"info\",\"debug\",\"verbose\"]}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_enum_mismatch(void)
{
    cJSON *value = make_str("critical");
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"string\","
        "\"enum\":[\"none\",\"error\",\"warn\",\"info\",\"debug\",\"verbose\"]}",
        value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_NOT_NULL(strstr(err.message, "critical"));
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// additionalProperties=false
// ---------------------------------------------------------------------------

void test_validate_additional_properties_false_ok(void)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "status", "rebooting");
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\","
        "\"properties\":{\"status\":{\"type\":\"string\"}},"
        "\"additionalProperties\":false}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_additional_properties_false_rejects_extra(void)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "status", "rebooting");
    cJSON_AddStringToObject(value, "extra_key", "forbidden");
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\","
        "\"properties\":{\"status\":{\"type\":\"string\"}},"
        "\"additionalProperties\":false}",
        value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_NOT_NULL(strstr(err.message, "extra_key"));
    cJSON_Delete(value);
}

void test_validate_additional_properties_default_allows_extra(void)
{
    // No additionalProperties in schema → extra keys allowed
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "status", "ok");
    cJSON_AddStringToObject(value, "bonus_key", "allowed");
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\","
        "\"properties\":{\"status\":{\"type\":\"string\"}}}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// Unknown keyword — warns but does not fail
// ---------------------------------------------------------------------------

void test_validate_unknown_keyword_does_not_fail(void)
{
    cJSON *value = make_str("hello");
    // "description" is not a validated keyword
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"string\",\"description\":\"some text\"}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// Unknown `type` value (e.g. "type":"weirdo") — validator warns + accepts.
void test_validate_unknown_type_value_passes(void)
{
    cJSON *value = make_str("anything");
    bb_err_t rc = bb_openapi_validate("{\"type\":\"weirdo\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// Enum mismatch with a numeric value — exercises the %g formatter branch in
// the error renderer.
void test_validate_enum_numeric_mismatch_renders_value(void)
{
    cJSON *value = make_int(42);
    bb_openapi_validate_err_t err = {0};
    bb_err_t rc = bb_openapi_validate(
        "{\"enum\":[1,2,3]}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_NOT_NULL(strstr(err.message, "42"));
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// Production schema smoke tests
// ---------------------------------------------------------------------------

// Mirrors POST /api/reboot response schema from
// platform/espidf/bb_system/bb_system_routes.c
void test_validate_smoke_reboot_schema(void)
{
    static const char schema[] =
        "{\"type\":\"object\","
        "\"properties\":{\"status\":{\"type\":\"string\"}},"
        "\"required\":[\"status\"]}";

    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "status", "rebooting");

    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(schema, value, &err);
    TEST_ASSERT_EQUAL_MESSAGE(BB_OK, rc, err.message);
    cJSON_Delete(value);
}

// Mirrors GET /api/ota/check 200 response schema from
// platform/espidf/bb_ota_pull/bb_ota_pull.c
void test_validate_smoke_ota_check_schema(void)
{
    static const char schema[] =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"current_version\":{\"type\":\"string\"},"
        "\"latest_version\":{\"type\":\"string\"},"
        "\"update_available\":{\"type\":\"boolean\"},"
        "\"asset\":{\"type\":\"string\"}},"
        "\"required\":[\"latest_version\",\"update_available\"]}";

    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "latest_version", "1.2.3");
    cJSON_AddTrueToObject(value, "update_available");

    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(schema, value, &err);
    TEST_ASSERT_EQUAL_MESSAGE(BB_OK, rc, err.message);
    cJSON_Delete(value);
}

// Mirrors POST /api/log/level request schema from
// platform/espidf/bb_log/bb_log_http.c
void test_validate_smoke_log_level_schema(void)
{
    static const char schema[] =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"tag\":{\"type\":\"string\"},"
        "\"level\":{\"type\":\"string\","
        "\"enum\":[\"none\",\"error\",\"warn\",\"info\",\"debug\",\"verbose\"]}},"
        "\"required\":[\"tag\",\"level\"]}";

    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "tag", "app");
    cJSON_AddStringToObject(value, "level", "debug");

    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(schema, value, &err);
    TEST_ASSERT_EQUAL_MESSAGE(BB_OK, rc, err.message);
    cJSON_Delete(value);
}

// Mirrors GET /api/diag/panic 200 response schema from
// platform/espidf/bb_diag/bb_diag_routes.c — exercises nested items
void test_validate_smoke_panic_schema(void)
{
    static const char schema[] =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"available\":{\"type\":\"boolean\"},"
        "\"boots_since\":{\"type\":\"integer\"},"
        "\"reset_reason\":{\"type\":\"string\"},"
        "\"log_tail\":{\"type\":\"string\"},"
        "\"task\":{\"type\":\"string\"},"
        "\"exc_pc\":{\"type\":\"integer\"},"
        "\"exc_cause\":{\"type\":\"integer\"},"
        "\"backtrace\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}},"
        "\"required\":[\"available\"]}";

    cJSON *value = cJSON_CreateObject();
    cJSON_AddTrueToObject(value, "available");
    cJSON_AddNumberToObject(value, "boots_since", 3);
    cJSON_AddStringToObject(value, "reset_reason", "panic");
    cJSON *bt = cJSON_CreateArray();
    cJSON_AddItemToArray(bt, cJSON_CreateNumber(0x400d1234));
    cJSON_AddItemToArray(bt, cJSON_CreateNumber(0x400d5678));
    cJSON_AddItemToObject(value, "backtrace", bt);

    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate(schema, value, &err);
    TEST_ASSERT_EQUAL_MESSAGE(BB_OK, rc, err.message);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// err=NULL branches for enum, required, properties, items
// ---------------------------------------------------------------------------

// enum mismatch with err=NULL — must not crash, must return VALIDATION.
void test_validate_enum_mismatch_null_err(void)
{
    cJSON *value = make_str("bad");
    bb_err_t rc = bb_openapi_validate("{\"enum\":[\"a\",\"b\"]}", value, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

// required mismatch with err=NULL — must not crash, must return VALIDATION.
void test_validate_required_missing_null_err(void)
{
    cJSON *value = cJSON_CreateObject();
    bb_err_t rc = bb_openapi_validate(
        "{\"required\":[\"x\"]}", value, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

// additionalProperties=false rejection with err=NULL — must not crash.
void test_validate_additional_properties_rejection_null_err(void)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "extra", "forbidden");
    bb_err_t rc = bb_openapi_validate(
        "{\"properties\":{},"
        "\"additionalProperties\":false}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// Malformed keyword branches (non-conforming keyword type — ignored)
// ---------------------------------------------------------------------------

// required is a string, not array — keyword ignored, validation passes.
void test_validate_malformed_required_non_array_ignored(void)
{
    cJSON *value = cJSON_CreateObject();
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\",\"required\":\"must-be-array\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// required array contains a non-string item — that item is skipped.
void test_validate_required_non_string_item_skipped(void)
{
    // required: [42, "x"] — the 42 is skipped; "x" IS present, so OK.
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "x", "present");
    bb_err_t rc = bb_openapi_validate(
        "{\"required\":[42,\"x\"]}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// properties is a number, not object — keyword ignored, validation passes.
void test_validate_malformed_properties_non_object_ignored(void)
{
    cJSON *value = cJSON_CreateObject();
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\",\"properties\":42}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// type keyword is a number, not string — keyword ignored, validation passes.
void test_validate_malformed_type_non_string_ignored(void)
{
    cJSON *value = make_str("hello");
    bb_err_t rc = bb_openapi_validate("{\"type\":42}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// enum is a string, not array — keyword ignored, validation passes.
void test_validate_malformed_enum_non_array_ignored(void)
{
    cJSON *value = make_str("anything");
    bb_err_t rc = bb_openapi_validate("{\"enum\":\"not-an-array\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// check_required: value is not an object — keyword skipped (type handles it)
// ---------------------------------------------------------------------------

void test_validate_required_non_object_value_skipped(void)
{
    // required is present but value is a string, not object — skipped by design.
    cJSON *value = make_str("not-an-object");
    bb_err_t rc = bb_openapi_validate(
        "{\"required\":[\"x\"]}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// check_properties: value is not an object — keyword skipped
// ---------------------------------------------------------------------------

void test_validate_properties_non_object_value_skipped(void)
{
    // properties present but value is a string — skipped.
    cJSON *value = make_str("not-an-object");
    bb_err_t rc = bb_openapi_validate(
        "{\"properties\":{\"x\":{\"type\":\"string\"}}}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// check_properties: additionalProperties=true — extra keys allowed explicitly
// ---------------------------------------------------------------------------

void test_validate_additional_properties_true_allows_extra(void)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "known", "val");
    cJSON_AddStringToObject(value, "extra", "also-ok");
    bb_err_t rc = bb_openapi_validate(
        "{\"properties\":{\"known\":{\"type\":\"string\"}},"
        "\"additionalProperties\":true}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// check_items: value is not an array — keyword skipped
// ---------------------------------------------------------------------------

void test_validate_items_non_array_value_skipped(void)
{
    // items present but value is a string — skipped.
    cJSON *value = make_str("not-an-array");
    bb_err_t rc = bb_openapi_validate(
        "{\"items\":{\"type\":\"string\"}}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// check_enum: empty enum array — no match possible, returns VALIDATION
// ---------------------------------------------------------------------------

void test_validate_enum_empty_always_fails(void)
{
    cJSON *value = make_str("anything");
    bb_openapi_validate_err_t err = {0};
    bb_err_t rc = bb_openapi_validate("{\"enum\":[]}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// check_enum: non-string non-number value renders <non-string> in message
// ---------------------------------------------------------------------------

void test_validate_enum_bool_value_renders_non_string(void)
{
    cJSON *value = make_bool(true);
    bb_openapi_validate_err_t err = {0};
    bb_err_t rc = bb_openapi_validate("{\"enum\":[\"a\",\"b\"]}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_NOT_NULL(strstr(err.message, "non-string"));
    cJSON_Delete(value);
}

void test_validate_enum_null_value_renders_non_string(void)
{
    cJSON *value = cJSON_CreateNull();
    bb_openapi_validate_err_t err = {0};
    bb_err_t rc = bb_openapi_validate("{\"enum\":[\"a\",\"b\"]}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    TEST_ASSERT_NOT_NULL(strstr(err.message, "non-string"));
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// validate_node: schema is not an object (e.g. a JSON boolean) — always passes
// ---------------------------------------------------------------------------

void test_validate_schema_not_object_passes(void)
{
    // JSON Schema boolean schemas: "true" means always-valid.
    // Our validator treats non-object schemas as always-pass.
    cJSON *value = make_str("anything");
    bb_err_t rc = bb_openapi_validate("true", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_schema_array_not_object_passes(void)
{
    cJSON *value = make_int(1);
    bb_err_t rc = bb_openapi_validate("[1,2,3]", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// Deep path: object→array→object transition exercises path stack fully
// ---------------------------------------------------------------------------

void test_validate_deep_path_object_array_object(void)
{
    // Schema: {type:object, properties:{items:{type:array, items:{type:object,
    //   required:["val"]}}}}
    // Value:  {items: [{val:1}, {}]}  — second element missing "val"
    // Expected error path: "items.[1]"
    cJSON *elem0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(elem0, "val", 1);
    cJSON *elem1 = cJSON_CreateObject();  // missing "val"

    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, elem0);
    cJSON_AddItemToArray(arr, elem1);

    cJSON *value = cJSON_CreateObject();
    cJSON_AddItemToObject(value, "items", arr);

    bb_openapi_validate_err_t err = {0};
    bb_err_t rc = bb_openapi_validate(
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"items\":{\"type\":\"array\","
        "\"items\":{\"type\":\"object\","
        "\"required\":[\"val\"]}}}}",
        value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    // path should contain "items" and "[1]"
    TEST_ASSERT_NOT_NULL(strstr(err.path, "items"));
    TEST_ASSERT_NOT_NULL(strstr(err.path, "[1]"));
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// type: "number" — match and mismatch
// ---------------------------------------------------------------------------

void test_validate_type_number_match(void)
{
    cJSON *value = cJSON_CreateNumber(3.14);
    bb_err_t rc = bb_openapi_validate("{\"type\":\"number\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_type_number_mismatch(void)
{
    cJSON *value = make_str("not-a-number");
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate("{\"type\":\"number\"}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// type: "null" — match and mismatch
// ---------------------------------------------------------------------------

void test_validate_type_null_match(void)
{
    cJSON *value = cJSON_CreateNull();
    bb_err_t rc = bb_openapi_validate("{\"type\":\"null\"}", value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

void test_validate_type_null_mismatch(void)
{
    cJSON *value = make_str("not-null");
    bb_openapi_validate_err_t err;
    bb_err_t rc = bb_openapi_validate("{\"type\":\"null\"}", value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// path_render truncation: segment longer than available buffer space
// ---------------------------------------------------------------------------

void test_validate_path_render_truncation(void)
{
    // path[64]: after writing a 1-char outer key ("a") + dot, avail = 61.
    // A 62-char inner key triggers seg_len (62) > avail (61) in path_render.
    // path_push clips segments to 63 chars, so 62 chars passes through unclipped.
    static const char outer_key[] = "a";
    // 62 chars:
    static const char inner_key[] =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    char schema[512];
    snprintf(schema, sizeof(schema),
             "{\"properties\":{\"%s\":{\"properties\":{\"%s\":"
             "{\"type\":\"integer\"}}}}}",
             outer_key, inner_key);

    cJSON *inner_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(inner_obj, inner_key, "wrong-type");
    cJSON *value = cJSON_CreateObject();
    cJSON_AddItemToObject(value, outer_key, inner_obj);

    bb_openapi_validate_err_t err = {0};
    bb_err_t rc = bb_openapi_validate(schema, value, &err);
    // Type violation detected; path is truncated but must not crash.
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

// path_render: buffer fills before all segments rendered.
// Exercises the `written < bufsz - 1` short-circuit-false branch in the for header.
void test_validate_path_render_buffer_full_midloop(void)
{
    // 30-char keys nested 4 deep. After iteration 2, written hits bufsz-1 (63);
    // iteration 3 then trips `written < bufsz - 1` false to exit the loop.
    static const char k[] = "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"; // 30 chars

    char schema[1024];
    snprintf(schema, sizeof(schema),
             "{\"properties\":{\"%s\":{\"properties\":{\"%s\":"
             "{\"properties\":{\"%s\":{\"properties\":{\"%s\":"
             "{\"type\":\"integer\"}}}}}}}}}",
             k, k, k, k);

    cJSON *l4 = cJSON_CreateObject();
    cJSON_AddStringToObject(l4, k, "wrong-type");
    cJSON *l3 = cJSON_CreateObject();
    cJSON_AddItemToObject(l3, k, l4);
    cJSON *l2 = cJSON_CreateObject();
    cJSON_AddItemToObject(l2, k, l3);
    cJSON *value = cJSON_CreateObject();
    cJSON_AddItemToObject(value, k, l2);

    bb_openapi_validate_err_t err = {0};
    bb_err_t rc = bb_openapi_validate(schema, value, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// path_push_index overflow: arrays nested > 16 levels
// ---------------------------------------------------------------------------

void test_validate_path_push_index_overflow(void)
{
    // Build a schema: {type:array, items:{type:array, ...}} 20 levels deep,
    // innermost item is {type:string}.
    // Value: [[[...[42]...]]] — 20 levels, innermost is wrong type.
    // path_push_index no-ops at depth >= PATH_DEPTH_MAX; must not crash.
    static const char schema[] =
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"array\",\"items\":"
        "{\"type\":\"string\"}"
        "}}}}}}}}}}}}}}}}}}";

    // Build value: 17 nested arrays, innermost contains an integer (mismatch).
    cJSON *inner = cJSON_CreateArray();
    cJSON_AddItemToArray(inner, make_int(42));
    for (int i = 0; i < 16; i++) {
        cJSON *outer = cJSON_CreateArray();
        cJSON_AddItemToArray(outer, inner);
        inner = outer;
    }

    bb_openapi_validate_err_t err = {0};
    bb_err_t rc = bb_openapi_validate(schema, inner, &err);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(inner);
}

// ---------------------------------------------------------------------------
// additionalProperties with non-boolean value — cJSON_IsBool false-branch
// ---------------------------------------------------------------------------

void test_validate_additional_properties_non_bool_ignored(void)
{
    // "additionalProperties": "invalid" — not a bool, so reject_extra stays false.
    // Extra key should be allowed.
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "known", "val");
    cJSON_AddStringToObject(value, "extra", "allowed");
    bb_err_t rc = bb_openapi_validate(
        "{\"properties\":{\"known\":{\"type\":\"string\"}},"
        "\"additionalProperties\":\"invalid\"}",
        value, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    cJSON_Delete(value);
}

// ---------------------------------------------------------------------------
// path_push overflow: filling the path stack beyond PATH_DEPTH_MAX (16)
// ---------------------------------------------------------------------------

void test_validate_path_stack_overflow_does_not_crash(void)
{
    // Build a deeply nested object+schema that pushes the path stack past 16.
    // Each level: {type:object, properties:{a:{...}}}
    // Value: {a:{a:{a:...}}} with 20 levels, innermost "a" is wrong type.
    // The push at depth 16 silently no-ops; validation still proceeds.
    static const char schema[] =
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"object\",\"properties\":{\"a\":"
        "{\"type\":\"integer\"}"
        "}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}";

    // Build matching value: 18 levels of {a: ...}, innermost is string (mismatch)
    cJSON *inner = make_str("wrong-type");
    for (int i = 0; i < 17; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddItemToObject(obj, "a", inner);
        inner = obj;
    }

    bb_openapi_validate_err_t err = {0};
    bb_err_t rc = bb_openapi_validate(schema, inner, &err);
    // Should detect the type violation (even if path is truncated at depth 16)
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
    cJSON_Delete(inner);
}
