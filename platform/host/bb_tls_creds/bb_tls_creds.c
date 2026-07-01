/* bb_tls_creds — portable TLS credential resolver.
 *
 * Compiled for both host (test) and ESP-IDF targets.  All NVS access goes
 * through the portable bb_nv_get_str API; no raw nvs_* calls here.
 */
#include "bb_tls_creds.h"
#include "bb_nv.h"
#include "bb_nv_keys.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>

/* Weak default definitions so the linker is satisfied when no consumer
 * provides embedded credentials.  Resolves to NULL / 0 when not overridden. */
const char   bb_tls_creds_embedded_ca[]      __attribute__((weak)) = {0};
const size_t bb_tls_creds_embedded_ca_len    __attribute__((weak)) = 0;
const char   bb_tls_creds_embedded_cert[]    __attribute__((weak)) = {0};
const size_t bb_tls_creds_embedded_cert_len  __attribute__((weak)) = 0;
const char   bb_tls_creds_embedded_key[]     __attribute__((weak)) = {0};
const size_t bb_tls_creds_embedded_key_len   __attribute__((weak)) = 0;

static const char *TAG = "bb_tls_creds";

/* Maximum PEM size accepted from NVS.  On ESP-IDF the Kconfig knob
 * CONFIG_BB_TLS_CREDS_NVS_MAX_LEN drives this; host tests fall back to 4096. */
#ifndef CONFIG_BB_TLS_CREDS_NVS_MAX_LEN
#define CONFIG_BB_TLS_CREDS_NVS_MAX_LEN 4096
#endif
#define BB_TLS_CREDS_NVS_MAX_LEN CONFIG_BB_TLS_CREDS_NVS_MAX_LEN

/* ---------------------------------------------------------------------------
 * Testing allocator injection (BB_TLS_CREDS_TESTING only).
 * Allows host tests to inject a failing malloc to exercise OOM paths.
 * ---------------------------------------------------------------------------*/
#ifdef BB_TLS_CREDS_TESTING
static bb_tls_creds_malloc_fn s_malloc = malloc;
void bb_tls_creds_set_malloc(bb_tls_creds_malloc_fn fn) { s_malloc = fn ? fn : malloc; }
void bb_tls_creds_reset_malloc(void)                    { s_malloc = malloc; }
#define TLS_MALLOC(n) s_malloc(n)
#else
#define TLS_MALLOC(n) malloc(n)
#endif /* BB_TLS_CREDS_TESTING */

/* Copy src (len bytes) into a newly heap-allocated NUL-terminated buffer.
 * Returns NULL on allocation failure. */
static char *dup_pem(const char *src, size_t len)
{
    char *buf = (char *)TLS_MALLOC(len + 1);
    if (!buf) return NULL;
    memcpy(buf, src, len);
    buf[len] = '\0';
    return buf;
}

/* Attempt to resolve one credential slot.  Returns BB_OK on success, or
 * BB_ERR_NO_SPACE on allocation failure.  *out_ptr / *out_len are unchanged
 * when no credential is found (caller pre-zeroes). */
static bb_err_t resolve_one(const char *override_pem,
                             const char *ns,
                             const char *nv_key,
                             const char *embedded_data,
                             size_t      embedded_len,
                             char      **out_ptr,
                             size_t     *out_len)
{
    /* All three paths below report length INCLUDING the NUL terminator so that
     * mbedtls_x509_crt_parse (PEM path) receives the NUL it requires.
     *
     * 1. Programmatic override */
    if (override_pem != NULL) {
        size_t len = strlen(override_pem);
        *out_ptr = dup_pem(override_pem, len);
        if (!*out_ptr) return BB_ERR_NO_SPACE;
        *out_len = len + 1;  /* include NUL — mbedtls PEM parse requires it */
        return BB_OK;
    }

    /* 2. NVS lookup — heap-allocate the read buffer so resolve_one does not
     * push 4 KiB (BB_TLS_CREDS_NVS_MAX_LEN) onto the caller's stack.  On
     * constrained targets (e.g. CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584) a
     * 4 KiB stack buffer here overflows the entire main-task stack and causes
     * a FreeRTOS stack-overflow crash at PRE_HTTP init (hardware-confirmed). */
    if (ns != NULL) {
        char *buf = (char *)TLS_MALLOC(BB_TLS_CREDS_NVS_MAX_LEN);
        if (!buf) return BB_ERR_NO_SPACE;
        bb_err_t rc = bb_nv_get_str(ns, nv_key, buf, BB_TLS_CREDS_NVS_MAX_LEN, NULL);
        if (rc == BB_OK && buf[0] != '\0') {
            size_t len = strlen(buf);
            *out_ptr = dup_pem(buf, len);
            free(buf);
            if (!*out_ptr) return BB_ERR_NO_SPACE;
            *out_len = len + 1;  /* include NUL — mbedtls PEM parse requires it */
            return BB_OK;
        }
        free(buf);
    }

    /* 3. Embedded weak default — skip when len is 0 (not overridden).
     * EMBED_TXTFILES appends a NUL and counts it in _len, so embedded_len
     * already includes the NUL — consistent with paths 1 and 2 above. */
    if (embedded_len > 0 && embedded_data != NULL) {
        *out_ptr = dup_pem(embedded_data, embedded_len);
        if (!*out_ptr) return BB_ERR_NO_SPACE;
        *out_len = embedded_len;
        return BB_OK;
    }

    /* Credential absent — leave *out_ptr NULL */
    return BB_OK;
}

bb_err_t bb_tls_creds_resolve(const char *ns, const bb_tls_creds_cfg_t *over,
                               bb_tls_creds_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    const char *ov_ca   = over ? over->ca_pem          : NULL;
    const char *ov_cert = over ? over->client_cert_pem : NULL;
    const char *ov_key  = over ? over->client_key_pem  : NULL;

    bb_err_t rc;

    rc = resolve_one(ov_ca, ns, BB_NV_KEY_TLS_CA,
                     bb_tls_creds_embedded_ca, bb_tls_creds_embedded_ca_len,
                     &out->ca, &out->ca_len);
    if (rc != BB_OK) {
        bb_log_e(TAG, "failed to resolve CA credential");
        bb_tls_creds_free(out);
        return rc;
    }

    // Client cert and key: only resolved when BB_TLS_MUTUAL_ENABLE is on.
    // When the gate is OFF the fields stay NULL/0 (zeroed by memset above),
    // saving NVS lookups and heap allocation on plaintext / CA-only builds.
#if CONFIG_BB_TLS_MUTUAL_ENABLE
    rc = resolve_one(ov_cert, ns, BB_NV_KEY_TLS_CERT,
                     bb_tls_creds_embedded_cert, bb_tls_creds_embedded_cert_len,
                     &out->cert, &out->cert_len);
    if (rc != BB_OK) {
        bb_log_e(TAG, "failed to resolve client cert credential");
        bb_tls_creds_free(out);
        return rc;
    }

    rc = resolve_one(ov_key, ns, BB_NV_KEY_TLS_KEY,
                     bb_tls_creds_embedded_key, bb_tls_creds_embedded_key_len,
                     &out->key, &out->key_len);
    if (rc != BB_OK) {
        bb_log_e(TAG, "failed to resolve client key credential");
        bb_tls_creds_free(out);
        return rc;
    }
#else
    (void)ov_cert;
    (void)ov_key;
#endif /* CONFIG_BB_TLS_MUTUAL_ENABLE */

    return BB_OK;
}

void bb_tls_creds_free(bb_tls_creds_t *c)
{
    if (!c) return;
    free(c->ca);
    free(c->cert);
    free(c->key);
    memset(c, 0, sizeof(*c));
}
