#include "unity.h"
#include "bb_json.h"

#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Basic object round-trip (string / number / bool / null)
// ---------------------------------------------------------------------------

void test_bb_json_obj_string_roundtrip(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_json_obj_set_string(obj, "name", "acme-corp");

    char *s = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(s);

    bb_json_t parsed = bb_json_parse(s, 0);
    TEST_ASSERT_NOT_NULL(parsed);

    char buf[64];
    bool ok = bb_json_obj_get_string(parsed, "name", buf, sizeof(buf));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("acme-corp", buf);

    bb_json_free_str(s);
    bb_json_free(parsed);
    bb_json_free(obj);
}

void test_bb_json_obj_number_roundtrip(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_number(obj, "count", 42.0);

    char *s = bb_json_serialize(obj);
    bb_json_t parsed = bb_json_parse(s, 0);

    double val = 0.0;
    bool ok = bb_json_obj_get_number(parsed, "count", &val);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_DOUBLE(42.0, val);

    bb_json_free_str(s);
    bb_json_free(parsed);
    bb_json_free(obj);
}

void test_bb_json_obj_bool_true_roundtrip(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_bool(obj, "active", true);

    char *s = bb_json_serialize(obj);
    bb_json_t parsed = bb_json_parse(s, 0);

    bool val = false;
    bool ok = bb_json_obj_get_bool(parsed, "active", &val);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(val);

    bb_json_free_str(s);
    bb_json_free(parsed);
    bb_json_free(obj);
}

void test_bb_json_obj_bool_false_roundtrip(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_bool(obj, "enabled", false);

    char *s = bb_json_serialize(obj);
    bb_json_t parsed = bb_json_parse(s, 0);

    bool val = true;
    bool ok = bb_json_obj_get_bool(parsed, "enabled", &val);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(val);

    bb_json_free_str(s);
    bb_json_free(parsed);
    bb_json_free(obj);
}

void test_bb_json_obj_null_roundtrip(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_null(obj, "data");

    char *s = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(s);
    // The serialised string must contain the key with null value
    TEST_ASSERT_NOT_NULL(strstr(s, "\"data\":null"));

    bb_json_free_str(s);
    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// Multiple fields in one object
// ---------------------------------------------------------------------------

void test_bb_json_obj_multiple_fields(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_string(obj, "user", "test-user@example.com");
    bb_json_obj_set_number(obj, "id", 99.0);
    bb_json_obj_set_bool(obj, "admin", false);

    char *s = bb_json_serialize(obj);
    bb_json_t parsed = bb_json_parse(s, 0);

    char ubuf[64];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "user", ubuf, sizeof(ubuf)));
    TEST_ASSERT_EQUAL_STRING("test-user@example.com", ubuf);

    double id = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "id", &id));
    TEST_ASSERT_EQUAL_DOUBLE(99.0, id);

    bool admin = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "admin", &admin));
    TEST_ASSERT_FALSE(admin);

    bb_json_free_str(s);
    bb_json_free(parsed);
    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// Nested object-in-object
// ---------------------------------------------------------------------------

void test_bb_json_nested_obj_in_obj(void)
{
    bb_json_t inner = bb_json_obj_new();
    bb_json_obj_set_string(inner, "host", "example.com");
    bb_json_obj_set_number(inner, "port", 443.0);

    bb_json_t outer = bb_json_obj_new();
    bb_json_obj_set_string(outer, "service", "https");
    bb_json_obj_set_obj(outer, "endpoint", inner); // inner owned by outer now

    char *s = bb_json_serialize(outer);
    TEST_ASSERT_NOT_NULL(s);
    // Round-trip the whole thing
    bb_json_t parsed = bb_json_parse(s, 0);
    TEST_ASSERT_NOT_NULL(parsed);

    char svcbuf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "service", svcbuf, sizeof(svcbuf)));
    TEST_ASSERT_EQUAL_STRING("https", svcbuf);

    bb_json_free_str(s);
    bb_json_free(parsed);
    bb_json_free(outer); // also frees inner
}

// ---------------------------------------------------------------------------
// Array-in-object
// ---------------------------------------------------------------------------

void test_bb_json_arr_in_obj(void)
{
    bb_json_t arr = bb_json_arr_new();
    bb_json_arr_append_string(arr, "alpha");
    bb_json_arr_append_string(arr, "beta");

    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_arr(obj, "tags", arr); // arr owned by obj now

    char *s = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"tags\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"alpha\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"beta\""));

    bb_json_free_str(s);
    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// Array of strings
// ---------------------------------------------------------------------------

void test_bb_json_arr_of_strings(void)
{
    bb_json_t arr = bb_json_arr_new();
    bb_json_arr_append_string(arr, "one");
    bb_json_arr_append_string(arr, "two");
    bb_json_arr_append_string(arr, "three");

    char *s = bb_json_serialize(arr);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"one\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"two\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"three\""));

    bb_json_free_str(s);
    bb_json_free(arr);
}

// ---------------------------------------------------------------------------
// Array of numbers
// ---------------------------------------------------------------------------

void test_bb_json_arr_of_numbers(void)
{
    bb_json_t arr = bb_json_arr_new();
    bb_json_arr_append_number(arr, 1.0);
    bb_json_arr_append_number(arr, 2.5);
    bb_json_arr_append_number(arr, -3.0);

    char *s = bb_json_serialize(arr);
    TEST_ASSERT_NOT_NULL(s);

    bb_json_free_str(s);
    bb_json_free(arr);
}

// ---------------------------------------------------------------------------
// Array of objects
// ---------------------------------------------------------------------------

void test_bb_json_arr_of_objects(void)
{
    bb_json_t arr = bb_json_arr_new();

    bb_json_t item1 = bb_json_obj_new();
    bb_json_obj_set_string(item1, "id", "tk-test-000");
    bb_json_arr_append_obj(arr, item1);

    bb_json_t item2 = bb_json_obj_new();
    bb_json_obj_set_string(item2, "id", "tk-test-001");
    bb_json_arr_append_obj(arr, item2);

    char *s = bb_json_serialize(arr);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "tk-test-000"));
    TEST_ASSERT_NOT_NULL(strstr(s, "tk-test-001"));

    bb_json_free_str(s);
    bb_json_free(arr);
}

// ---------------------------------------------------------------------------
// Numeric edge cases
// ---------------------------------------------------------------------------

void test_bb_json_number_zero(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_number(obj, "val", 0.0);

    char *s = bb_json_serialize(obj);
    bb_json_t p = bb_json_parse(s, 0);
    double v = 1.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(p, "val", &v));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, v);

    bb_json_free_str(s);
    bb_json_free(p);
    bb_json_free(obj);
}

void test_bb_json_number_negative(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_number(obj, "temp", -273.15);

    char *s = bb_json_serialize(obj);
    bb_json_t p = bb_json_parse(s, 0);
    double v = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(p, "temp", &v));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, -273.15, v);

    bb_json_free_str(s);
    bb_json_free(p);
    bb_json_free(obj);
}

void test_bb_json_number_large(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_number(obj, "big", 1234567890.0);

    char *s = bb_json_serialize(obj);
    bb_json_t p = bb_json_parse(s, 0);
    double v = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(p, "big", &v));
    TEST_ASSERT_EQUAL_DOUBLE(1234567890.0, v);

    bb_json_free_str(s);
    bb_json_free(p);
    bb_json_free(obj);
}

void test_bb_json_number_decimal(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_number(obj, "pi", 3.14159265358979);

    char *s = bb_json_serialize(obj);
    bb_json_t p = bb_json_parse(s, 0);
    double v = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(p, "pi", &v));
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 3.14159, v);

    bb_json_free_str(s);
    bb_json_free(p);
    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// Missing key getters return false
// ---------------------------------------------------------------------------

void test_bb_json_get_missing_string_returns_false(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_string(obj, "present", "value");
    char *s = bb_json_serialize(obj);
    bb_json_t p = bb_json_parse(s, 0);

    char buf[32];
    TEST_ASSERT_FALSE(bb_json_obj_get_string(p, "absent", buf, sizeof(buf)));

    bb_json_free_str(s);
    bb_json_free(p);
    bb_json_free(obj);
}

void test_bb_json_get_missing_number_returns_false(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_number(obj, "x", 1.0);
    char *s = bb_json_serialize(obj);
    bb_json_t p = bb_json_parse(s, 0);

    double v = 0.0;
    TEST_ASSERT_FALSE(bb_json_obj_get_number(p, "y", &v));

    bb_json_free_str(s);
    bb_json_free(p);
    bb_json_free(obj);
}

void test_bb_json_get_missing_bool_returns_false(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_bool(obj, "flag", true);
    char *s = bb_json_serialize(obj);
    bb_json_t p = bb_json_parse(s, 0);

    bool v = false;
    TEST_ASSERT_FALSE(bb_json_obj_get_bool(p, "other", &v));

    bb_json_free_str(s);
    bb_json_free(p);
    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// Parse invalid JSON returns NULL
// ---------------------------------------------------------------------------

void test_bb_json_parse_invalid_returns_null(void)
{
    bb_json_t p = bb_json_parse("{not valid", 0);
    TEST_ASSERT_NULL(p);
}

// ---------------------------------------------------------------------------
// Parse with explicit length
// ---------------------------------------------------------------------------

void test_bb_json_parse_with_length(void)
{
    // Valid JSON followed by garbage — only parse the valid part via len
    const char text[] = "{\"k\":\"v\"}GARBAGE";
    bb_json_t p = bb_json_parse(text, 9); // 9 == strlen("{\"k\":\"v\"}")
    TEST_ASSERT_NOT_NULL(p);

    char buf[16];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(p, "k", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("v", buf);

    bb_json_free(p);
}

// ---------------------------------------------------------------------------
// Serialize NULL returns NULL
// ---------------------------------------------------------------------------

void test_bb_json_serialize_null_returns_null(void)
{
    char *s = bb_json_serialize(NULL);
    TEST_ASSERT_NULL(s);
}

// ---------------------------------------------------------------------------
// Type mismatch: getter returns false when key exists but wrong type
// ---------------------------------------------------------------------------

void test_bb_json_type_mismatch_string_vs_number(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_number(obj, "count", 5.0);
    char *s = bb_json_serialize(obj);
    bb_json_t p = bb_json_parse(s, 0);

    char buf[16];
    // Trying to get a number key as string should fail
    TEST_ASSERT_FALSE(bb_json_obj_get_string(p, "count", buf, sizeof(buf)));

    bb_json_free_str(s);
    bb_json_free(p);
    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// Raw item access (B1-61)
// ---------------------------------------------------------------------------

void test_bb_json_obj_get_item_returns_handle(void)
{
    bb_json_t p = bb_json_parse("{\"k\":\"v\"}", 0);
    bb_json_t item = bb_json_obj_get_item(p, "k");
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_TRUE(bb_json_item_is_string(item));
    TEST_ASSERT_EQUAL_STRING("v", bb_json_item_get_string(item));
    TEST_ASSERT_NULL(bb_json_obj_get_item(p, "missing"));
    bb_json_free(p);
}

void test_bb_json_arr_size_and_get_item(void)
{
    bb_json_t p = bb_json_parse("[\"a\",42,true,null]", 0);
    TEST_ASSERT_EQUAL_INT(4, bb_json_arr_size(p));

    TEST_ASSERT_TRUE(bb_json_item_is_string(bb_json_arr_get_item(p, 0)));
    TEST_ASSERT_EQUAL_STRING("a", bb_json_item_get_string(bb_json_arr_get_item(p, 0)));

    TEST_ASSERT_TRUE(bb_json_item_is_number(bb_json_arr_get_item(p, 1)));
    TEST_ASSERT_EQUAL_INT(42, bb_json_item_get_int(bb_json_arr_get_item(p, 1)));
    TEST_ASSERT_EQUAL_DOUBLE(42.0, bb_json_item_get_double(bb_json_arr_get_item(p, 1)));

    TEST_ASSERT_TRUE(bb_json_item_is_true(bb_json_arr_get_item(p, 2)));
    TEST_ASSERT_TRUE(bb_json_item_is_null(bb_json_arr_get_item(p, 3)));

    TEST_ASSERT_NULL(bb_json_arr_get_item(p, 99));
    bb_json_free(p);
}

void test_bb_json_item_is_array_and_object(void)
{
    bb_json_t p = bb_json_parse("{\"arr\":[1,2],\"nested\":{\"x\":1}}", 0);
    bb_json_t arr = bb_json_obj_get_item(p, "arr");
    bb_json_t nested = bb_json_obj_get_item(p, "nested");

    TEST_ASSERT_TRUE(bb_json_item_is_array(arr));
    TEST_ASSERT_FALSE(bb_json_item_is_object(arr));
    TEST_ASSERT_TRUE(bb_json_item_is_object(nested));
    TEST_ASSERT_FALSE(bb_json_item_is_array(nested));

    TEST_ASSERT_EQUAL_INT(2, bb_json_arr_size(arr));
    bb_json_free(p);
}

void test_bb_json_item_serialize_subtree(void)
{
    bb_json_t p = bb_json_parse("{\"nested\":{\"x\":1}}", 0);
    bb_json_t nested = bb_json_obj_get_item(p, "nested");
    char *s = bb_json_item_serialize(nested);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"x\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "1"));
    bb_json_free_str(s);
    bb_json_free(p);
}

void test_bb_json_item_null_handle_is_safe(void)
{
    TEST_ASSERT_FALSE(bb_json_item_is_string(NULL));
    TEST_ASSERT_FALSE(bb_json_item_is_number(NULL));
    TEST_ASSERT_FALSE(bb_json_item_is_array(NULL));
    TEST_ASSERT_FALSE(bb_json_item_is_object(NULL));
    TEST_ASSERT_FALSE(bb_json_item_is_true(NULL));
    TEST_ASSERT_TRUE(bb_json_item_is_null(NULL));
    TEST_ASSERT_NULL(bb_json_item_get_string(NULL));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, bb_json_item_get_double(NULL));
    TEST_ASSERT_EQUAL_INT(0, bb_json_item_get_int(NULL));
    TEST_ASSERT_NULL(bb_json_item_serialize(NULL));
    TEST_ASSERT_EQUAL_INT(0, bb_json_arr_size(NULL));
    TEST_ASSERT_NULL(bb_json_arr_get_item(NULL, 0));
    TEST_ASSERT_NULL(bb_json_obj_get_item(NULL, "k"));
}

// ---------------------------------------------------------------------------
// bb_json_arr_append_string_n
// ---------------------------------------------------------------------------

void test_bb_json_arr_append_string_n_basic(void)
{
    bb_json_t arr = bb_json_arr_new();
    TEST_ASSERT_NOT_NULL(arr);

    // Append a substring: "hello" from "hello world"
    bb_json_arr_append_string_n(arr, "hello world", 5);
    // Append a single-char string
    bb_json_arr_append_string_n(arr, "xy", 1);

    TEST_ASSERT_EQUAL_INT(2, bb_json_arr_size(arr));

    bb_json_t item0 = bb_json_arr_get_item(arr, 0);
    TEST_ASSERT_TRUE(bb_json_item_is_string(item0));
    TEST_ASSERT_EQUAL_STRING("hello", bb_json_item_get_string(item0));

    bb_json_t item1 = bb_json_arr_get_item(arr, 1);
    TEST_ASSERT_TRUE(bb_json_item_is_string(item1));
    TEST_ASSERT_EQUAL_STRING("x", bb_json_item_get_string(item1));

    bb_json_free(arr);
}

void test_bb_json_arr_append_string_n_null_arr_is_safe(void)
{
    // Must not crash
    bb_json_arr_append_string_n(NULL, "abc", 3);
}

void test_bb_json_arr_append_string_n_null_str_is_safe(void)
{
    bb_json_t arr = bb_json_arr_new();
    TEST_ASSERT_NOT_NULL(arr);
    // Must not crash; nothing appended
    bb_json_arr_append_string_n(arr, NULL, 3);
    TEST_ASSERT_EQUAL_INT(0, bb_json_arr_size(arr));
    bb_json_free(arr);
}
