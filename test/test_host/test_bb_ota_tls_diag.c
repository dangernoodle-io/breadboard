#include "unity.h"
#include "bb_ota_pull.h"
#include <string.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// bb_ota_tls_diag — pure TLS handshake diagnostic helper (B1-358)
// ---------------------------------------------------------------------------

void test_bb_ota_tls_diag_symptom_code_returns_true(void)
{
    char buf[256];
    bool r = bb_ota_tls_diag(-0x7200, "github.com", 4096, buf, sizeof(buf));
    TEST_ASSERT_TRUE(r);
}

void test_bb_ota_tls_diag_symptom_code_contains_host(void)
{
    char buf[256];
    bb_ota_tls_diag(-0x7200, "github.com", 4096, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "github.com"));
}

void test_bb_ota_tls_diag_symptom_code_contains_ssl_in_len(void)
{
    char buf[256];
    bb_ota_tls_diag(-0x7200, "github.com", 4096, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "4096"));
}

void test_bb_ota_tls_diag_symptom_code_contains_config_key(void)
{
    char buf[256];
    bb_ota_tls_diag(-0x7200, "github.com", 4096, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN"));
}

void test_bb_ota_tls_diag_non_symptom_code_returns_false(void)
{
    char buf[256];
    bool r = bb_ota_tls_diag(-0x0050, "example.com", 16384, buf, sizeof(buf));
    TEST_ASSERT_FALSE(r);
}

void test_bb_ota_tls_diag_non_symptom_code_non_empty(void)
{
    char buf[256];
    bb_ota_tls_diag(-0x0050, "example.com", 16384, buf, sizeof(buf));
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_ota_tls_diag_non_symptom_code_contains_host(void)
{
    char buf[256];
    bb_ota_tls_diag(-0x0050, "example.com", 16384, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "example.com"));
}

void test_bb_ota_tls_diag_null_out_does_not_crash(void)
{
    // must not crash; return value is still meaningful
    bool r = bb_ota_tls_diag(-0x7200, "example.com", 4096, NULL, 0);
    TEST_ASSERT_TRUE(r);
}

void test_bb_ota_tls_diag_null_out_non_symptom_does_not_crash(void)
{
    bool r = bb_ota_tls_diag(-0x0050, "example.com", 4096, NULL, 0);
    TEST_ASSERT_FALSE(r);
}

void test_bb_ota_tls_diag_short_buf_does_not_crash(void)
{
    char buf[4];
    // snprintf truncates; must not crash, buf is NUL-terminated
    bb_ota_tls_diag(-0x7200, "example.com", 4096, buf, sizeof(buf));
    // buf[sizeof(buf)-1] is guaranteed NUL by snprintf
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
}

void test_bb_ota_tls_diag_zero_out_len_does_not_crash(void)
{
    char buf[4] = "xyz";
    bb_ota_tls_diag(-0x7200, "example.com", 4096, buf, 0);
    // buf must be untouched (out_len == 0 → no-op)
    TEST_ASSERT_EQUAL_CHAR('x', buf[0]);
}

void test_bb_ota_tls_diag_null_host_does_not_crash(void)
{
    char buf[256];
    bool r = bb_ota_tls_diag(-0x7200, NULL, 4096, buf, sizeof(buf));
    TEST_ASSERT_TRUE(r);
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}
