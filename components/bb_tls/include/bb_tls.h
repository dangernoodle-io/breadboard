#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "bb_core.h"

/* ---------------------------------------------------------------------------
 * Kconfig bridge — CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
 * --------------------------------------------------------------------------- */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
#define BB_TLS_SSL_IN_LEN CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
#endif
#endif
#ifndef BB_TLS_SSL_IN_LEN
#define BB_TLS_SSL_IN_LEN 4096  /* sane default when not configured */
#endif
#define BB_TLS_SSL_IN_FLOOR (BB_TLS_SSL_IN_LEN + 1024)

/* ---------------------------------------------------------------------------
 * Kconfig bridge — BB_TLS_HEAP_CONTIGUOUS_FLOOR
 * --------------------------------------------------------------------------- */
#ifdef ESP_PLATFORM
#ifdef CONFIG_BB_TLS_HEAP_CONTIGUOUS_FLOOR
#define BB_TLS_HEAP_CONTIGUOUS_FLOOR CONFIG_BB_TLS_HEAP_CONTIGUOUS_FLOOR
#endif
#endif
#ifndef BB_TLS_HEAP_CONTIGUOUS_FLOOR
#define BB_TLS_HEAP_CONTIGUOUS_FLOOR 0
#endif

/* ---------------------------------------------------------------------------
 * Kconfig bridge — BB_TLS_HEAP_TOTAL_FLOOR
 * --------------------------------------------------------------------------- */
#ifdef ESP_PLATFORM
#ifdef CONFIG_BB_TLS_HEAP_TOTAL_FLOOR
#define BB_TLS_HEAP_TOTAL_FLOOR CONFIG_BB_TLS_HEAP_TOTAL_FLOOR
#endif
#endif
#ifndef BB_TLS_HEAP_TOTAL_FLOOR
#define BB_TLS_HEAP_TOTAL_FLOOR 0
#endif

/* MBEDTLS_ERR_SSL_RECORD_TOO_BIG = -0x7200 */
#define BB_TLS_RECORD_TOO_BIG (-0x7200)

/* ---------------------------------------------------------------------------
 * Portable TLS handshake failure classification (B1-362)
 * --------------------------------------------------------------------------- */

/**
 * Coarse classification of a TLS handshake failure for diagnostic reporting.
 * portable — valid on espidf and host; NONE/0 when no failure.
 */
typedef enum {
    BB_TLS_FAIL_NONE         = 0,  // no failure / not yet classified
    BB_TLS_FAIL_RECORD_TOO_BIG,   // MBEDTLS_ERR_SSL_RECORD_TOO_BIG (-0x7200)
    BB_TLS_FAIL_OTHER,             // any other mbedtls handshake error
} bb_tls_fail_t;

/**
 * Classify an mbedtls error code into a portable bb_tls_fail_t bucket.
 *
 * Pure — no ESP-IDF headers, no stdlib beyond the constant comparison.
 * Compiled on host and device.
 *
 * @param mbedtls_err  mbedtls error code (0 → NONE; -0x7200 → RECORD_TOO_BIG; else OTHER)
 * @return bb_tls_fail_t classification
 */
bb_tls_fail_t bb_tls_classify(int mbedtls_err);

/**
 * Classify an mbedtls error from a failed TLS handshake and fill a
 * caller-supplied buffer with an actionable human-readable message.
 *
 * When the error code matches the mbedtls record-size symptom (-0x7200),
 * the message names the endpoint HOST and the current SSL_IN_CONTENT_LEN
 * value and tells the operator to increase CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN.
 * For other codes a compact generic message is produced.
 *
 * @param mbedtls_err  mbedtls error code (negative int)
 * @param host         NUL-terminated hostname; may be NULL (shown as "?")
 * @param ssl_in_len   current CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN value (bytes)
 * @param out          caller-supplied output buffer; may be NULL (no-op)
 * @param out_len      size of out; may be 0 (no-op)
 * @return true when mbedtls_err matches the record-size symptom (-0x7200)
 *
 * No ESP-IDF headers; no stdlib beyond snprintf. Compiled on host and device.
 */
bool bb_tls_handshake_diag(int mbedtls_err, const char *host, int ssl_in_len,
                            char *out, size_t out_len);

/**
 * Pure pre-flight heap guard predicate — no ESP-IDF dependencies.
 *
 * Returns true when both heap dimensions clear their floors (TLS may proceed).
 * Returns false when either dimension is below its floor; *out_dim is set to
 * "contiguous" or "total-free" to identify which dimension tripped.
 *
 * Floor semantics:
 *   contiguous_floor == 0  → disabled (auto-derive caller-side using BB_TLS_SSL_IN_FLOOR)
 *   contiguous_floor >  0  → explicit byte floor
 *   contiguous_floor <  0  → disabled (guard never fires)
 *   total_floor      == 0  → disabled
 *   total_floor      >  0  → explicit byte floor
 *
 * @param largest_block    measured largest contiguous free block (bytes)
 * @param contiguous_floor minimum required contiguous block (see semantics above)
 * @param total_free       measured total free heap (bytes)
 * @param total_floor      minimum required total free heap (0 = disabled)
 * @param out_dim          on failure, points to a string literal; may be NULL
 */
bool bb_tls_heap_guard_passes(size_t largest_block, size_t contiguous_floor,
                               size_t total_free, size_t total_floor,
                               const char **out_dim);
