// test_meta_fragment_selftest -- direct unit coverage of
// test_meta_fragment_extract_properties() (test_meta_fragment.h/.c) itself,
// independent of the production golden tests that consume it
// (test_bb_mqtt_client_health_section_meta_golden.c,
// test_bb_temp_health_meta_golden.c -- those only ever pass it well-formed
// schemas and assert true). Every early-return false path gets its own
// case here, plus the "fails safe" contract: `out` is never partially
// written on a false return (the implementation only ever memcpy()s once,
// after every check has passed), proven by pre-filling `out` with a
// sentinel and asserting it's untouched.

#include "unity.h"

#include "test_meta_fragment.h"

#include <string.h>

// Sentinel fill so a false-return path's "no partial/oob write" claim is
// actually checked, not just assumed.
static void fill_sentinel(char *buf, size_t len)
{
    memset(buf, 0x5A, len);
}

static void assert_untouched(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x5A, (unsigned char)buf[i]);
    }
}

/* ---- argument validation (NULL schema / NULL out / zero out_size) ---- */

void test_meta_fragment_extract_properties_null_schema_returns_false(void)
{
    char out[64];
    fill_sentinel(out, sizeof out);

    TEST_ASSERT_FALSE(test_meta_fragment_extract_properties(NULL, out, sizeof out));
    assert_untouched(out, sizeof out);
}

void test_meta_fragment_extract_properties_null_out_returns_false(void)
{
    static const char *const schema = "{\"properties\":{\"a\":{\"type\":\"boolean\"}}}";

    TEST_ASSERT_FALSE(test_meta_fragment_extract_properties(schema, NULL, 64));
}

void test_meta_fragment_extract_properties_zero_out_size_returns_false(void)
{
    static const char *const schema = "{\"properties\":{\"a\":{\"type\":\"boolean\"}}}";
    char out[64];
    fill_sentinel(out, sizeof out);

    TEST_ASSERT_FALSE(test_meta_fragment_extract_properties(schema, out, 0));
    assert_untouched(out, sizeof out);
}

/* ---- "properties" key missing / value not an object ---- */

void test_meta_fragment_extract_properties_missing_key_returns_false(void)
{
    static const char *const schema = "{\"type\":\"object\"}";
    char out[64];
    fill_sentinel(out, sizeof out);

    TEST_ASSERT_FALSE(test_meta_fragment_extract_properties(schema, out, sizeof out));
    assert_untouched(out, sizeof out);
}

void test_meta_fragment_extract_properties_value_not_object_returns_false(void)
{
    static const char *const schema = "{\"properties\":\"not-an-object\"}";
    char out[64];
    fill_sentinel(out, sizeof out);

    TEST_ASSERT_FALSE(test_meta_fragment_extract_properties(schema, out, sizeof out));
    assert_untouched(out, sizeof out);
}

/* ---- unbalanced/truncated object (also exercises the nested-object
 * depth>0 "}" branch that never fires in any flat golden schema) ---- */

void test_meta_fragment_extract_properties_unbalanced_braces_returns_false(void)
{
    // Inner object ("b") closes (depth 2 -> 1, NOT the depth==0 case), but
    // the outer "properties" object never does -- string ends mid-object.
    static const char *const schema = "{\"properties\":{\"a\":{\"b\":1}";
    char out[64];
    fill_sentinel(out, sizeof out);

    TEST_ASSERT_FALSE(test_meta_fragment_extract_properties(schema, out, sizeof out));
    assert_untouched(out, sizeof out);
}

/* ---- dangling backslash at end of input: exercises the escape check's
 * false branch (c == '\\' && p[1] != '\0') and leaves the scan inside an
 * unterminated string, so it also never balances ---- */

void test_meta_fragment_extract_properties_dangling_escape_returns_false(void)
{
    static const char *const schema = "{\"properties\":{\"a\":\"x\\";
    char out[64];
    fill_sentinel(out, sizeof out);

    TEST_ASSERT_FALSE(test_meta_fragment_extract_properties(schema, out, sizeof out));
    assert_untouched(out, sizeof out);
}

/* ---- escaped quote inside a string value: exercises the escape check's
 * true branch (c == '\\' && p[1] != '\0') on a well-formed, successfully
 * extracted fragment ---- */

void test_meta_fragment_extract_properties_escaped_quote_extracts_successfully(void)
{
    static const char *const schema =
        "{\"properties\":{\"a\":{\"description\":\"say \\\"hi\\\"\"}},"
        "\"required\":[]}";
    static const char *const expected =
        "\"properties\":{\"a\":{\"description\":\"say \\\"hi\\\"\"}}";
    char out[128];

    TEST_ASSERT_TRUE(test_meta_fragment_extract_properties(schema, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

/* ---- output buffer too small for the extracted span ---- */

void test_meta_fragment_extract_properties_out_buffer_too_small_returns_false(void)
{
    static const char *const schema = "{\"properties\":{\"a\":{\"type\":\"boolean\"}}}";
    char out[4];
    fill_sentinel(out, sizeof out);

    TEST_ASSERT_FALSE(test_meta_fragment_extract_properties(schema, out, sizeof out));
    assert_untouched(out, sizeof out);
}

/* ---- baseline success case, standalone from the production goldens ---- */

void test_meta_fragment_extract_properties_flat_object_succeeds(void)
{
    static const char *const schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"enabled\":{\"type\":\"boolean\"},"
        "\"connected\":{\"type\":\"boolean\"}},"
        "\"required\":[],\"additionalProperties\":false}";
    static const char *const expected =
        "\"properties\":{"
        "\"enabled\":{\"type\":\"boolean\"},"
        "\"connected\":{\"type\":\"boolean\"}}";
    char out[128];

    TEST_ASSERT_TRUE(test_meta_fragment_extract_properties(schema, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING(expected, out);
}
