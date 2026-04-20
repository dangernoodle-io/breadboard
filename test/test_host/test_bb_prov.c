#include "unity.h"
#include "bb_prov.h"
#include <string.h>

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
