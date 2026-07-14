#include "unity.h"
#include "bb_tls_creds.h"
#include "bb_config.h"
#include "bb_storage.h"
#include "bb_nv_keys.h"
#include "fake_nvs_backend.h"
#include <string.h>
#include <stdlib.h>

/* B1-756 (bb_nv dissolution epic B1-708): bb_tls_creds now reads NVS PEMs
 * via bb_config_get_str (backend="nvs") instead of bb_nv_get_str -- host
 * tests exercise that path against fake_nvs_backend.h's fake "nvs" backend
 * (the real "nvs" bb_storage backend is ESP-IDF-only), mirroring
 * test_bb_settings_flags.c's reset_state()/seed pattern. Registered fresh
 * per test (bb_storage_test_reset() + re-register) rather than relying on
 * test_main.c's global setUp(), since bb_storage's backend registry is a
 * single global singleton shared by every test file linked into this
 * binary. */
static void reset_state(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
}

/* Seed a PEM string at (ns, key) via the SAME "nvs" backend address
 * bb_tls_creds_resolve()'s per-call bb_config_field_t targets -- proves the
 * production read path against data written through the facade, not a
 * bb_nv-specific side door. */
static void seed_nvs(const char *ns, const char *key, const char *value)
{
    const bb_config_field_t f = {
        .id      = "test.tls_pem",
        .type    = BB_CONFIG_STR,
        .addr    = { .backend = "nvs", .ns_or_dir = ns, .key = key },
        .max_len = 4096,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&f, value));
}

/* ---- Programmatic override takes priority over NVS ---- */

void test_bb_tls_creds_override_ca_beats_nvs(void)
{
    reset_state();
    /* Store something in NVS — override should win */
    seed_nvs("test_ns", BB_NV_KEY_TLS_CA, "-----BEGIN CERTIFICATE-----\nnvs_ca\n-----END CERTIFICATE-----\n");

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
    reset_state();
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
    reset_state();
    seed_nvs("mqtt_ns", BB_NV_KEY_TLS_CA,   "ca_from_nvs");
    seed_nvs("mqtt_ns", BB_NV_KEY_TLS_CERT, "cert_from_nvs");
    seed_nvs("mqtt_ns", BB_NV_KEY_TLS_KEY,  "key_from_nvs");

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
    reset_state();
    seed_nvs("partial_ns", BB_NV_KEY_TLS_CA, "only_ca");

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
    reset_state();
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
    reset_state();
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
    reset_state();
    bb_err_t rc = bb_tls_creds_resolve("ns", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

/* ---- bb_tls_creds_free zeroes and double-free safe ---- */

void test_bb_tls_creds_free_zeroes_fields(void)
{
    reset_state();
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
    reset_state();
    /* Must not crash */
    bb_tls_creds_free(NULL);
}

void test_bb_tls_creds_free_double_free_safe(void)
{
    reset_state();
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
    reset_state();
    /* Write to NVS, resolve, then overwrite NVS — resolved buffer must be unaffected */
    seed_nvs("copy_ns", BB_NV_KEY_TLS_CA, "original_ca");

    bb_tls_creds_t creds = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_tls_creds_resolve("copy_ns", NULL, &creds));
    TEST_ASSERT_NOT_NULL(creds.ca);
    TEST_ASSERT_EQUAL_STRING("original_ca", creds.ca);

    /* Overwrite NVS entry — creds.ca must still hold the original copy */
    seed_nvs("copy_ns", BB_NV_KEY_TLS_CA, "replaced_ca");
    TEST_ASSERT_EQUAL_STRING("original_ca", creds.ca);

    bb_tls_creds_free(&creds);
}

void test_bb_tls_creds_override_buffer_is_copy(void)
{
    reset_state();
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
    reset_state();
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
    reset_state();
    const char *pem = "-----BEGIN CERTIFICATE-----\nNVS\n-----END CERTIFICATE-----\n";
    seed_nvs("nvs_ns", BB_NV_KEY_TLS_CA, pem);

    bb_tls_creds_t creds = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_tls_creds_resolve("nvs_ns", NULL, &creds));
    TEST_ASSERT_NOT_NULL(creds.ca);
    TEST_ASSERT_EQUAL_UINT(strlen(pem) + 1, creds.ca_len);
    TEST_ASSERT_EQUAL_STRING(pem, creds.ca);
    bb_tls_creds_free(&creds);
}

/* ---- Heap-buffer alloc failure in the NVS path (stack-overflow fix) ---- */

/* The NVS read buffer is now heap-allocated (BB_TLS_CREDS_NVS_MAX_LEN = 4096).
 * On constrained targets the old 4 KiB stack buf overflowed the entire main
 * task (CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584).  Inject a failing malloc on the
 * NVS buf alloc and verify BB_ERR_NO_SPACE is returned with no leak or crash. */
static void *failing_malloc(size_t sz) { (void)sz; return NULL; }

void test_bb_tls_creds_nvs_buf_alloc_fails_returns_no_space(void)
{
    reset_state();
    seed_nvs("alloc_fail_ns", BB_NV_KEY_TLS_CA, "some_ca_pem");

    bb_tls_creds_set_malloc(failing_malloc);
    bb_tls_creds_t creds = {0};
    bb_err_t rc = bb_tls_creds_resolve("alloc_fail_ns", NULL, &creds);
    bb_tls_creds_reset_malloc();

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    /* no partial allocation should have escaped */
    TEST_ASSERT_NULL(creds.ca);
    TEST_ASSERT_NULL(creds.cert);
    TEST_ASSERT_NULL(creds.key);
}

/* ---- Cert/key resolve failure propagates from bb_tls_creds_resolve
 * (CONFIG_BB_TLS_MUTUAL_ENABLE=1 in the native test env) ----
 *
 * These are pre-existing bb_tls_creds_resolve error-propagation branches
 * (rc != BB_OK after the cert/key resolve_one() calls) -- previously
 * baselined as uncovered gaps at their old line numbers; this migration's
 * unrelated edit (the NUL-termination fix above) shifted them to new line
 * numbers, which coverage_baseline.py's per-line ratchet flags as "new"
 * (B1-929 borrowed coverage). Backfilled with real BITE coverage here
 * rather than re-baselined. A counting malloc lets the FIRST (CA) resolve
 * succeed while forcing a LATER call to fail, isolating the cert-specific
 * and key-specific error paths from each other and from the
 * already-covered CA failure path (test_bb_tls_creds_nvs_buf_alloc_fails_
 * returns_no_space above, which fails on the very first call). */
static int  s_counting_malloc_calls;
static int  s_counting_malloc_fail_after; /* fail starting at this 1-based call */

static void *counting_malloc(size_t sz)
{
    s_counting_malloc_calls++;
    if (s_counting_malloc_calls >= s_counting_malloc_fail_after) {
        return NULL;
    }
    return malloc(sz);
}

void test_bb_tls_creds_cert_resolve_failure_propagates(void)
{
    reset_state();

    /* CA resolved via override (1 malloc, succeeds). Cert/key have no
     * override, so both resolve via the ns=NULL-store NVS path (buf alloc
     * is call #2 for cert) -- failing call #2 fails ONLY the cert resolve. */
    s_counting_malloc_calls = 0;
    s_counting_malloc_fail_after = 2;
    bb_tls_creds_set_malloc(counting_malloc);

    bb_tls_creds_cfg_t over = { .ca_pem = "ca_ok", .client_cert_pem = NULL, .client_key_pem = NULL };
    bb_tls_creds_t creds = {0};
    bb_err_t rc = bb_tls_creds_resolve("cert_fail_ns", &over, &creds);

    bb_tls_creds_reset_malloc();

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    /* bb_tls_creds_free() inside the error path must have released the
     * already-resolved CA and zeroed everything -- no partial state leaks. */
    TEST_ASSERT_NULL(creds.ca);
    TEST_ASSERT_NULL(creds.cert);
    TEST_ASSERT_NULL(creds.key);
}

void test_bb_tls_creds_key_resolve_failure_propagates(void)
{
    reset_state();

    /* CA via override (call #1, succeeds). Cert via the ns NVS path with
     * nothing stored -- buf alloc is call #2 (succeeds), and since the
     * store is empty (buf[0]=='\0') no second dup_pem malloc happens for
     * cert, so key's buf alloc is call #3 -- failing call #3 fails ONLY
     * the key resolve. */
    s_counting_malloc_calls = 0;
    s_counting_malloc_fail_after = 3;
    bb_tls_creds_set_malloc(counting_malloc);

    bb_tls_creds_cfg_t over = { .ca_pem = "ca_ok", .client_cert_pem = NULL, .client_key_pem = NULL };
    bb_tls_creds_t creds = {0};
    bb_err_t rc = bb_tls_creds_resolve("key_fail_ns", &over, &creds);

    bb_tls_creds_reset_malloc();

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_NULL(creds.ca);
    TEST_ASSERT_NULL(creds.cert);
    TEST_ASSERT_NULL(creds.key);
}
