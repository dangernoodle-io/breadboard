#include "unity.h"
#include "bb_tls_creds.h"
#include "bb_nv.h"
#include <string.h>
#include <stdlib.h>

/* setUp/tearDown are called per-test by the test runner via test_main.c setUp().
 * bb_nv_host_str_store_reset() is called there, so NVS state is clean per test. */

/* ---- Programmatic override takes priority over NVS ---- */

void test_bb_tls_creds_override_ca_beats_nvs(void)
{
    /* Store something in NVS — override should win */
    bb_nv_set_str("test_ns", "tls_ca", "-----BEGIN CERTIFICATE-----\nnvs_ca\n-----END CERTIFICATE-----\n");

    bb_tls_creds_cfg_t over = {
        .ca_pem          = "-----BEGIN CERTIFICATE-----\noverride_ca\n-----END CERTIFICATE-----\n",
        .client_cert_pem = NULL,
        .client_key_pem  = NULL,
    };
    bb_tls_creds_t creds = {0};
    bb_err_t rc = bb_tls_creds_resolve("test_ns", &over, &creds);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NOT_NULL(creds.ca);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(creds.ca, "override_ca"), "expected override CA");
    TEST_ASSERT_NULL(creds.cert);
    TEST_ASSERT_NULL(creds.key);
    bb_tls_creds_free(&creds);
}

void test_bb_tls_creds_override_all_fields(void)
{
    bb_tls_creds_cfg_t over = {
        .ca_pem          = "ca_pem_data",
        .client_cert_pem = "cert_pem_data",
        .client_key_pem  = "key_pem_data",
    };
    bb_tls_creds_t creds = {0};
    bb_err_t rc = bb_tls_creds_resolve("test_ns", &over, &creds);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NOT_NULL(creds.ca);
    TEST_ASSERT_NOT_NULL(creds.cert);
    TEST_ASSERT_NOT_NULL(creds.key);
    TEST_ASSERT_EQUAL_STRING("ca_pem_data",   creds.ca);
    TEST_ASSERT_EQUAL_STRING("cert_pem_data", creds.cert);
    TEST_ASSERT_EQUAL_STRING("key_pem_data",  creds.key);
    /* lengths include NUL so mbedtls_x509_crt_parse accepts them */
    TEST_ASSERT_EQUAL_UINT(strlen("ca_pem_data")   + 1, creds.ca_len);
    TEST_ASSERT_EQUAL_UINT(strlen("cert_pem_data") + 1, creds.cert_len);
    TEST_ASSERT_EQUAL_UINT(strlen("key_pem_data")  + 1, creds.key_len);
    bb_tls_creds_free(&creds);
}

/* ---- NVS used when no override ---- */

void test_bb_tls_creds_nvs_used_when_no_override(void)
{
    bb_nv_set_str("mqtt_ns", "tls_ca",   "ca_from_nvs");
    bb_nv_set_str("mqtt_ns", "tls_cert", "cert_from_nvs");
    bb_nv_set_str("mqtt_ns", "tls_key",  "key_from_nvs");

    bb_tls_creds_t creds = {0};
    bb_err_t rc = bb_tls_creds_resolve("mqtt_ns", NULL, &creds);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NOT_NULL(creds.ca);
    TEST_ASSERT_NOT_NULL(creds.cert);
    TEST_ASSERT_NOT_NULL(creds.key);
    TEST_ASSERT_EQUAL_STRING("ca_from_nvs",   creds.ca);
    TEST_ASSERT_EQUAL_STRING("cert_from_nvs", creds.cert);
    TEST_ASSERT_EQUAL_STRING("key_from_nvs",  creds.key);
    bb_tls_creds_free(&creds);
}

void test_bb_tls_creds_nvs_partial_only_ca(void)
{
    bb_nv_set_str("partial_ns", "tls_ca", "only_ca");

    bb_tls_creds_t creds = {0};
    bb_err_t rc = bb_tls_creds_resolve("partial_ns", NULL, &creds);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NOT_NULL(creds.ca);
    TEST_ASSERT_EQUAL_STRING("only_ca", creds.ca);
    TEST_ASSERT_NULL(creds.cert);
    TEST_ASSERT_NULL(creds.key);
    TEST_ASSERT_EQUAL_UINT(0, creds.cert_len);
    TEST_ASSERT_EQUAL_UINT(0, creds.key_len);
    bb_tls_creds_free(&creds);
}

void test_bb_tls_creds_nvs_skipped_when_ns_null(void)
{
    /* Nothing in NVS, no override, no embedded — all NULL */
    bb_tls_creds_t creds = {0};
    bb_err_t rc = bb_tls_creds_resolve(NULL, NULL, &creds);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NULL(creds.ca);
    TEST_ASSERT_NULL(creds.cert);
    TEST_ASSERT_NULL(creds.key);
    bb_tls_creds_free(&creds);
}

/* ---- Absent PEMs → NULL buffers, no crash ---- */

void test_bb_tls_creds_all_absent_returns_ok_null_buffers(void)
{
    /* No NVS, no override, no embedded — should succeed with all NULL */
    bb_tls_creds_t creds = {0};
    bb_err_t rc = bb_tls_creds_resolve("empty_ns", NULL, &creds);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NULL(creds.ca);
    TEST_ASSERT_NULL(creds.cert);
    TEST_ASSERT_NULL(creds.key);
    TEST_ASSERT_EQUAL_UINT(0, creds.ca_len);
    TEST_ASSERT_EQUAL_UINT(0, creds.cert_len);
    TEST_ASSERT_EQUAL_UINT(0, creds.key_len);
}

void test_bb_tls_creds_null_out_returns_invalid_arg(void)
{
    bb_err_t rc = bb_tls_creds_resolve("ns", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

/* ---- bb_tls_creds_free zeroes and double-free safe ---- */

void test_bb_tls_creds_free_zeroes_fields(void)
{
    bb_tls_creds_cfg_t over = {
        .ca_pem          = "ca_data",
        .client_cert_pem = "cert_data",
        .client_key_pem  = "key_data",
    };
    bb_tls_creds_t creds = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_tls_creds_resolve("ns", &over, &creds));
    TEST_ASSERT_NOT_NULL(creds.ca);

    bb_tls_creds_free(&creds);
    TEST_ASSERT_NULL(creds.ca);
    TEST_ASSERT_NULL(creds.cert);
    TEST_ASSERT_NULL(creds.key);
    TEST_ASSERT_EQUAL_UINT(0, creds.ca_len);
    TEST_ASSERT_EQUAL_UINT(0, creds.cert_len);
    TEST_ASSERT_EQUAL_UINT(0, creds.key_len);
}

void test_bb_tls_creds_free_null_is_safe(void)
{
    /* Must not crash */
    bb_tls_creds_free(NULL);
}

void test_bb_tls_creds_free_double_free_safe(void)
{
    bb_tls_creds_cfg_t over = { .ca_pem = "ca", .client_cert_pem = NULL, .client_key_pem = NULL };
    bb_tls_creds_t creds = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_tls_creds_resolve("ns", &over, &creds));
    bb_tls_creds_free(&creds);
    /* Second free on zeroed struct — must not crash */
    bb_tls_creds_free(&creds);
}

/* ---- Returned buffers are independent copies ---- */

void test_bb_tls_creds_buffers_are_independent_copies(void)
{
    /* Write to NVS, resolve, then overwrite NVS — resolved buffer must be unaffected */
    bb_nv_set_str("copy_ns", "tls_ca", "original_ca");

    bb_tls_creds_t creds = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_tls_creds_resolve("copy_ns", NULL, &creds));
    TEST_ASSERT_NOT_NULL(creds.ca);
    TEST_ASSERT_EQUAL_STRING("original_ca", creds.ca);

    /* Overwrite NVS entry — creds.ca must still hold the original copy */
    bb_nv_set_str("copy_ns", "tls_ca", "replaced_ca");
    TEST_ASSERT_EQUAL_STRING("original_ca", creds.ca);

    bb_tls_creds_free(&creds);
}

void test_bb_tls_creds_override_buffer_is_copy(void)
{
    /* Resolve from a stack-local override string, verify returned buffer is a
     * heap copy (independent of the override pointer's lifetime). */
    char override_buf[32];
    strcpy(override_buf, "stack_pem");

    bb_tls_creds_cfg_t over = { .ca_pem = override_buf, .client_cert_pem = NULL, .client_key_pem = NULL };
    bb_tls_creds_t creds = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_tls_creds_resolve("ns", &over, &creds));
    TEST_ASSERT_NOT_NULL(creds.ca);

    /* Mutate the source buffer — creds.ca must be unaffected */
    memset(override_buf, 0, sizeof(override_buf));
    TEST_ASSERT_EQUAL_STRING("stack_pem", creds.ca);

    bb_tls_creds_free(&creds);
}

/* ---- Lengths include NUL (mbedtls_x509_crt_parse PEM requirement) ---- */

void test_bb_tls_creds_override_len_includes_nul(void)
{
    const char *pem = "-----BEGIN CERTIFICATE-----\nABCD\n-----END CERTIFICATE-----\n";
    bb_tls_creds_cfg_t over = { .ca_pem = pem, .client_cert_pem = NULL, .client_key_pem = NULL };
    bb_tls_creds_t creds = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_tls_creds_resolve("ns", &over, &creds));
    TEST_ASSERT_EQUAL_UINT(strlen(pem) + 1, creds.ca_len);
    /* buffer is still a valid C string */
    TEST_ASSERT_EQUAL_STRING(pem, creds.ca);
    bb_tls_creds_free(&creds);
}

void test_bb_tls_creds_nvs_len_includes_nul(void)
{
    const char *pem = "-----BEGIN CERTIFICATE-----\nNVS\n-----END CERTIFICATE-----\n";
    bb_nv_set_str("nvs_ns", "tls_ca", pem);

    bb_tls_creds_t creds = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_tls_creds_resolve("nvs_ns", NULL, &creds));
    TEST_ASSERT_NOT_NULL(creds.ca);
    TEST_ASSERT_EQUAL_UINT(strlen(pem) + 1, creds.ca_len);
    TEST_ASSERT_EQUAL_STRING(pem, creds.ca);
    bb_tls_creds_free(&creds);
}
