#include "bb_tls.h"
#include <stdio.h>

bool bb_tls_handshake_diag(int mbedtls_err, const char *host, int ssl_in_len,
                            char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return (mbedtls_err == BB_TLS_RECORD_TOO_BIG);
    }
    if (mbedtls_err == BB_TLS_RECORD_TOO_BIG) {
        snprintf(out, out_len,
                 "OTA handshake to %s failed (mbedtls -0x7200); "
                 "SSL_IN_CONTENT_LEN (%d) may be too small for this endpoint's "
                 "certificate chain. Try increasing "
                 "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN.",
                 host ? host : "?", ssl_in_len);
        return true;
    }
    snprintf(out, out_len,
             "OTA handshake to %s failed (mbedtls 0x%04x)",
             host ? host : "?", (unsigned int)(mbedtls_err & 0xffff));
    return false;
}

bool bb_tls_heap_guard_passes(size_t largest_block, size_t contiguous_floor,
                               size_t total_free, size_t total_floor,
                               const char **out_dim)
{
    if (contiguous_floor > 0 && largest_block < contiguous_floor) {
        if (out_dim) *out_dim = "contiguous";
        return false;
    }
    if (total_floor > 0 && total_free < total_floor) {
        if (out_dim) *out_dim = "total-free";
        return false;
    }
    return true;
}

bb_tls_fail_t bb_tls_classify(int mbedtls_err)
{
    if (mbedtls_err == 0)              return BB_TLS_FAIL_NONE;
    if (mbedtls_err == BB_TLS_RECORD_TOO_BIG) return BB_TLS_FAIL_RECORD_TOO_BIG;
    return BB_TLS_FAIL_OTHER;
}
