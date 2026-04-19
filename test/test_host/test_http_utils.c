#include "unity.h"
#include "http_server.h"
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

void test_prov_parse_empty_body(void)
{
    char ssid[32], pass[64];
    bb_prov_parse_result_t result = bb_prov_parse_body("", 0, ssid, sizeof(ssid), pass, sizeof(pass));
    TEST_ASSERT_EQUAL(BB_PROV_PARSE_EMPTY_BODY, result);
}

void test_prov_parse_missing_ssid(void)
{
    char ssid[32], pass[64];
    const char *body = "pass=secret";
    bb_prov_parse_result_t result = bb_prov_parse_body(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    TEST_ASSERT_EQUAL(BB_PROV_PARSE_SSID_REQUIRED, result);
}

void test_prov_parse_ssid_only(void)
{
    char ssid[32], pass[64];
    const char *body = "ssid=test-net";
    bb_prov_parse_result_t result = bb_prov_parse_body(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    TEST_ASSERT_EQUAL(BB_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("test-net", ssid);
    TEST_ASSERT_EQUAL_STRING("", pass);
}

void test_prov_parse_ssid_and_pass(void)
{
    char ssid[32], pass[64];
    const char *body = "ssid=test-net&pass=hunter2";
    bb_prov_parse_result_t result = bb_prov_parse_body(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    TEST_ASSERT_EQUAL(BB_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("test-net", ssid);
    TEST_ASSERT_EQUAL_STRING("hunter2", pass);
}

void test_prov_parse_urlencoded_special(void)
{
    char ssid[32], pass[64];
    const char *body = "ssid=my%20net&pass=a%26b";
    bb_prov_parse_result_t result = bb_prov_parse_body(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    TEST_ASSERT_EQUAL(BB_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("my net", ssid);
    TEST_ASSERT_EQUAL_STRING("a&b", pass);
}
