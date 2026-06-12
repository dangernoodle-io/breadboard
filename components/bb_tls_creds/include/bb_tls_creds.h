#pragma once

#include <stddef.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Programmatic override: non-NULL PEM strings take priority over NVS and
 * embedded defaults.  Pass NULL for any field to skip that source.
 */
typedef struct {
    const char *ca_pem;
    const char *client_cert_pem;
    const char *client_key_pem;
} bb_tls_creds_cfg_t;

/**
 * Resolved TLS credentials.  Heap-owned; valid until bb_tls_creds_free().
 * Any of ca/cert/key may be NULL when that credential is absent.
 * Corresponding _len is 0 when the pointer is NULL.
 */
typedef struct {
    char  *ca;    size_t ca_len;
    char  *cert;  size_t cert_len;
    char  *key;   size_t key_len;
} bb_tls_creds_t;

/**
 * Resolve TLS credentials using three-level precedence:
 *   1. Programmatic override (over->*_pem when non-NULL)
 *   2. NVS strings in namespace `ns` (keys: tls_ca, tls_cert, tls_key)
 *   3. Build-time embedded weak defaults (bb_tls_creds_embedded_ca/_cert/_key)
 *
 * Each found PEM is copied into a freshly heap-allocated buffer owned by *out.
 * Call bb_tls_creds_free() to release.
 *
 * @param ns    NVS namespace to search (may be NULL to skip NVS lookup).
 * @param over  Programmatic overrides (may be NULL to skip).
 * @param out   Output struct; all fields zeroed on entry.
 * @return BB_OK on success (even when all three PEMs are absent).
 *         BB_ERR_NO_SPACE on allocation failure.
 */
bb_err_t bb_tls_creds_resolve(const char *ns, const bb_tls_creds_cfg_t *over,
                               bb_tls_creds_t *out);

/**
 * Free heap buffers owned by *c and zero all fields.
 * Safe to call with NULL or a zeroed struct (double-free safe).
 */
void bb_tls_creds_free(bb_tls_creds_t *c);

/* ---------------------------------------------------------------------------
 * Optional weak embedded defaults.
 *
 * A consumer (firmware) may define these symbols (e.g. via bb_embed_assets)
 * to supply build-time embedded PEM credentials.  bb_tls_creds_resolve()
 * uses them as a last resort after NVS lookup misses.
 *
 * Declare the _len symbol alongside the data symbol so the resolver knows
 * how many bytes to copy.  When the linker resolves to the weak NULL
 * definition the resolver skips that credential.
 * --------------------------------------------------------------------------- */
extern const char   bb_tls_creds_embedded_ca[]   __attribute__((weak));
extern const size_t bb_tls_creds_embedded_ca_len __attribute__((weak));
extern const char   bb_tls_creds_embedded_cert[] __attribute__((weak));
extern const size_t bb_tls_creds_embedded_cert_len __attribute__((weak));
extern const char   bb_tls_creds_embedded_key[]  __attribute__((weak));
extern const size_t bb_tls_creds_embedded_key_len __attribute__((weak));

#ifdef __cplusplus
}
#endif
