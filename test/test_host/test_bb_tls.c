#include "unity.h"
#include "bb_tls.h"
#include <string.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// bb_tls_handshake_diag — pure TLS handshake diagnostic helper
// ---------------------------------------------------------------------------

void test_bb_tls_handshake_diag_symptom_code_returns_true(void)
{
    char buf[256];
    bool r = bb_tls_handshake_diag(-0x7200, "github.com", 4096, buf, sizeof(buf));
    TEST_ASSERT_TRUE(r);
}

void test_bb_tls_handshake_diag_symptom_code_contains_host(void)
{
    char buf[256];
    bb_tls_handshake_diag(-0x7200, "github.com", 4096, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "github.com"));
}

void test_bb_tls_handshake_diag_symptom_code_contains_ssl_in_len(void)
{
    char buf[256];
    bb_tls_handshake_diag(-0x7200, "github.com", 4096, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "4096"));
}

void test_bb_tls_handshake_diag_symptom_code_contains_config_key(void)
{
    char buf[256];
    bb_tls_handshake_diag(-0x7200, "github.com", 4096, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN"));
}

void test_bb_tls_handshake_diag_non_symptom_code_returns_false(void)
{
    char buf[256];
    bool r = bb_tls_handshake_diag(-0x0050, "example.com", 16384, buf, sizeof(buf));
    TEST_ASSERT_FALSE(r);
}

void test_bb_tls_handshake_diag_non_symptom_code_non_empty(void)
{
    char buf[256];
    bb_tls_handshake_diag(-0x0050, "example.com", 16384, buf, sizeof(buf));
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

void test_bb_tls_handshake_diag_non_symptom_code_contains_host(void)
{
    char buf[256];
    bb_tls_handshake_diag(-0x0050, "example.com", 16384, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "example.com"));
}

void test_bb_tls_handshake_diag_null_out_does_not_crash(void)
{
    bool r = bb_tls_handshake_diag(-0x7200, "example.com", 4096, NULL, 0);
    TEST_ASSERT_TRUE(r);
}

void test_bb_tls_handshake_diag_null_out_non_symptom_does_not_crash(void)
{
    bool r = bb_tls_handshake_diag(-0x0050, "example.com", 4096, NULL, 0);
    TEST_ASSERT_FALSE(r);
}

void test_bb_tls_handshake_diag_short_buf_does_not_crash(void)
{
    char buf[4];
    bb_tls_handshake_diag(-0x7200, "example.com", 4096, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
}

void test_bb_tls_handshake_diag_zero_out_len_does_not_crash(void)
{
    char buf[4] = "xyz";
    bb_tls_handshake_diag(-0x7200, "example.com", 4096, buf, 0);
    TEST_ASSERT_EQUAL_CHAR('x', buf[0]);
}

void test_bb_tls_handshake_diag_null_host_does_not_crash(void)
{
    char buf[256];
    bool r = bb_tls_handshake_diag(-0x7200, NULL, 4096, buf, sizeof(buf));
    TEST_ASSERT_TRUE(r);
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

// ---------------------------------------------------------------------------
// bb_tls_heap_guard_passes — pure heap guard predicate
// ---------------------------------------------------------------------------

// both dimensions clear → pass
void test_bb_tls_heap_guard_passes_both_clear(void)
{
    const char *dim = NULL;
    bool r = bb_tls_heap_guard_passes(10000, 5000, 20000, 10000, &dim);
    TEST_ASSERT_TRUE(r);
    TEST_ASSERT_NULL(dim);
}

// contiguous dimension fails → false, dim = "contiguous"
void test_bb_tls_heap_guard_contiguous_fail(void)
{
    const char *dim = NULL;
    bool r = bb_tls_heap_guard_passes(4000, 5000, 20000, 10000, &dim);
    TEST_ASSERT_FALSE(r);
    TEST_ASSERT_NOT_NULL(dim);
    TEST_ASSERT_EQUAL_STRING("contiguous", dim);
}

// total-free dimension fails → false, dim = "total-free"
void test_bb_tls_heap_guard_total_fail(void)
{
    const char *dim = NULL;
    bool r = bb_tls_heap_guard_passes(10000, 5000, 5000, 10000, &dim);
    TEST_ASSERT_FALSE(r);
    TEST_ASSERT_NOT_NULL(dim);
    TEST_ASSERT_EQUAL_STRING("total-free", dim);
}

// disabled floor (0) → always passes that dimension
void test_bb_tls_heap_guard_disabled_contiguous_floor(void)
{
    const char *dim = NULL;
    // contiguous_floor=0 → disabled; even largest_block=0 passes
    bool r = bb_tls_heap_guard_passes(0, 0, 20000, 10000, &dim);
    TEST_ASSERT_TRUE(r);
    TEST_ASSERT_NULL(dim);
}

void test_bb_tls_heap_guard_disabled_total_floor(void)
{
    const char *dim = NULL;
    // total_floor=0 → disabled; even total_free=0 passes
    bool r = bb_tls_heap_guard_passes(10000, 5000, 0, 0, &dim);
    TEST_ASSERT_TRUE(r);
    TEST_ASSERT_NULL(dim);
}

// NULL out_dim safe on failure
void test_bb_tls_heap_guard_null_out_dim_safe(void)
{
    // must not crash; return value is still meaningful
    bool r = bb_tls_heap_guard_passes(100, 5000, 100, 5000, NULL);
    TEST_ASSERT_FALSE(r);
}
