#include "unity.h"
#include "bb_http.h"
#include <string.h>

void test_url_decode_basic(void)
{
    char out[256];
    bb_url_decode_field("ssid=TestNetwork&pass=secret", "ssid", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("TestNetwork", out);
}

void test_url_decode_plus_as_space(void)
{
    char out[256];
    bb_url_decode_field("ssid=Test+Network", "ssid", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Test Network", out);
}

void test_url_decode_hex_decode(void)
{
    char out[256];
    bb_url_decode_field("pass=hello%21world", "pass", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("hello!world", out);
}

void test_url_decode_missing_field(void)
{
    char out[256];
    bb_url_decode_field("ssid=TestNetwork", "pass", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_url_decode_truncation(void)
{
    char out[5];
    bb_url_decode_field("ssid=abcdefghij", "ssid", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("abcd", out);
}

void test_url_decode_percent_at_end(void)
{
    char out[256];
    bb_url_decode_field("f=abc%2", "f", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("abc%2", out);
}

void test_bb_url_decode_field_not_first(void)
{
    char out[256];
    bb_url_decode_field("a=1&b=2&c=3", "b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

void test_url_decode_field_not_first(void)
{
    char out[256];
    bb_url_decode_field("a=1&b=2&c=3", "b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

void test_url_decode_empty_value(void)
{
    char out[256];
    bb_url_decode_field("ssid=&pass=secret", "ssid", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_url_decode_field_at_end(void)
{
    char out[256];
    bb_url_decode_field("a=1&b=2", "b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

void test_bb_url_decode_field_at_end(void)
{
    char out[256];
    bb_url_decode_field("a=1&b=2", "b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2", out);
}

// bb_url_parse_bool tests
void test_bb_url_parse_bool_one(void)
{
    bool out = false;
    TEST_ASSERT_TRUE(bb_url_parse_bool("1", &out));
    TEST_ASSERT_TRUE(out);
}

void test_bb_url_parse_bool_zero(void)
{
    bool out = true;
    TEST_ASSERT_TRUE(bb_url_parse_bool("0", &out));
    TEST_ASSERT_FALSE(out);
}

void test_bb_url_parse_bool_true_lowercase(void)
{
    bool out = false;
    TEST_ASSERT_TRUE(bb_url_parse_bool("true", &out));
    TEST_ASSERT_TRUE(out);
}

void test_bb_url_parse_bool_true_uppercase(void)
{
    bool out = false;
    TEST_ASSERT_TRUE(bb_url_parse_bool("TRUE", &out));
    TEST_ASSERT_TRUE(out);
}

void test_bb_url_parse_bool_false_lowercase(void)
{
    bool out = true;
    TEST_ASSERT_TRUE(bb_url_parse_bool("false", &out));
    TEST_ASSERT_FALSE(out);
}

void test_bb_url_parse_bool_on(void)
{
    bool out = false;
    TEST_ASSERT_TRUE(bb_url_parse_bool("on", &out));
    TEST_ASSERT_TRUE(out);
}

void test_bb_url_parse_bool_off(void)
{
    bool out = true;
    TEST_ASSERT_TRUE(bb_url_parse_bool("off", &out));
    TEST_ASSERT_FALSE(out);
}

void test_bb_url_parse_bool_yes(void)
{
    bool out = false;
    TEST_ASSERT_TRUE(bb_url_parse_bool("yes", &out));
    TEST_ASSERT_TRUE(out);
}

void test_bb_url_parse_bool_no(void)
{
    bool out = true;
    TEST_ASSERT_TRUE(bb_url_parse_bool("no", &out));
    TEST_ASSERT_FALSE(out);
}

void test_bb_url_parse_bool_t(void)
{
    bool out = false;
    TEST_ASSERT_TRUE(bb_url_parse_bool("t", &out));
    TEST_ASSERT_TRUE(out);
}

void test_bb_url_parse_bool_f(void)
{
    bool out = true;
    TEST_ASSERT_TRUE(bb_url_parse_bool("f", &out));
    TEST_ASSERT_FALSE(out);
}

void test_bb_url_parse_bool_y(void)
{
    bool out = false;
    TEST_ASSERT_TRUE(bb_url_parse_bool("y", &out));
    TEST_ASSERT_TRUE(out);
}

void test_bb_url_parse_bool_n(void)
{
    bool out = true;
    TEST_ASSERT_TRUE(bb_url_parse_bool("n", &out));
    TEST_ASSERT_FALSE(out);
}

void test_bb_url_parse_bool_empty_string(void)
{
    bool out = true;
    TEST_ASSERT_FALSE(bb_url_parse_bool("", &out));
}

void test_bb_url_parse_bool_invalid_maybe(void)
{
    bool out = true;
    TEST_ASSERT_FALSE(bb_url_parse_bool("maybe", &out));
}

void test_bb_url_parse_bool_invalid_two(void)
{
    bool out = true;
    TEST_ASSERT_FALSE(bb_url_parse_bool("2", &out));
}

void test_bb_url_parse_bool_null_pointer(void)
{
    bool out = true;
    TEST_ASSERT_FALSE(bb_url_parse_bool(NULL, &out));
}

// Exercises istrcmp's "input shorter than candidate" exit:
// "tr" runs out before "true" does.
void test_bb_url_parse_bool_prefix_of_valid(void)
{
    bool out = true;
    TEST_ASSERT_FALSE(bb_url_parse_bool("tr", &out));
}

// Exercises istrcmp's "candidate shorter than input" exit:
// "trues" walks past the end of "true".
void test_bb_url_parse_bool_extension_of_valid(void)
{
    bool out = true;
    TEST_ASSERT_FALSE(bb_url_parse_bool("trues", &out));
}

// bb_url_parse_uint tests
void test_bb_url_parse_uint_zero(void)
{
    unsigned long out = 999;
    TEST_ASSERT_TRUE(bb_url_parse_uint("0", &out));
    TEST_ASSERT_EQUAL_UINT(0, out);
}

void test_bb_url_parse_uint_decimal_42(void)
{
    unsigned long out = 0;
    TEST_ASSERT_TRUE(bb_url_parse_uint("42", &out));
    TEST_ASSERT_EQUAL_UINT(42, out);
}

void test_bb_url_parse_uint_max_unsigned_long(void)
{
    unsigned long out = 0;
    TEST_ASSERT_TRUE(bb_url_parse_uint("4294967295", &out));
    TEST_ASSERT_EQUAL_UINT(4294967295U, out);
}

void test_bb_url_parse_uint_empty_string(void)
{
    unsigned long out = 999;
    TEST_ASSERT_FALSE(bb_url_parse_uint("", &out));
}

void test_bb_url_parse_uint_non_digit(void)
{
    unsigned long out = 999;
    TEST_ASSERT_FALSE(bb_url_parse_uint("abc", &out));
}

void test_bb_url_parse_uint_trailing_junk(void)
{
    unsigned long out = 999;
    TEST_ASSERT_FALSE(bb_url_parse_uint("12abc", &out));
}

void test_bb_url_parse_uint_leading_space(void)
{
    unsigned long out = 999;
    TEST_ASSERT_FALSE(bb_url_parse_uint(" 12", &out));
}

void test_bb_url_parse_uint_negative_sign(void)
{
    unsigned long out = 999;
    TEST_ASSERT_FALSE(bb_url_parse_uint("-1", &out));
}

void test_bb_url_parse_uint_decimal_point(void)
{
    unsigned long out = 999;
    TEST_ASSERT_FALSE(bb_url_parse_uint("1.5", &out));
}

void test_bb_url_parse_uint_null_pointer(void)
{
    unsigned long out = 999;
    TEST_ASSERT_FALSE(bb_url_parse_uint(NULL, &out));
}

// Exercises the ERANGE branch — strtoul saturates at ULONG_MAX and sets errno.
void test_bb_url_parse_uint_overflow(void)
{
    unsigned long out = 0;
    TEST_ASSERT_FALSE(bb_url_parse_uint("99999999999999999999", &out));
}
