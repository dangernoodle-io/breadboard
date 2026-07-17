// bb_tcp_client — portable connected TCP/TLS stream client.
//
/**
 * @brief Portable connected TCP/TLS stream client — the stream peer to
 * bb_udp_client. Flat per-platform-TU dispatch (see wiki Backend-Dispatch).
 */
//
// DUMB TRANSPORT (ouroboros KB decision): this component owns socket
// lifecycle and I/O only. It does NOT reconnect, back off, or keep-alive —
// that policy belongs to the consumer's state machine (e.g. bb_fsm driving
// a stratum client). Do not add retry logic here.
//
// Per-instance health (B1-1039): each pooled instance tracks its OWN
// connected/last_ok_ms/fail_count/tls_error_code — no shared/authoritative
// slot (bb_transport_health is no longer used by this component). Reporting
// policy — read this before wiring a consumer FSM off bb_tcp_client_health_fill():
//   - bb_tcp_client_connect() success  -> connected=true, last_ok_ms stamped
//   - bb_tcp_client_connect() failure  -> connected=false, fail_count++
//   - bb_tcp_client_read/write() returning a HARD transport error (reset,
//     closed, or any error other than a plain timeout) -> connected=false,
//     fail_count++
//   - a plain read timeout (BB_ERR_TIMEOUT, "no data yet") NEVER reports —
//     it is not a failure signal
//   - bb_tcp_client_close() clears connected without touching fail_count —
//     a clean close is not a transport failure
//   - tls_error_code is set from the TLS failure path (ESP-IDF backend) and
//     otherwise left at its last-reported value
//
// bb_tcp_client_health_fill() + bb_tcp_client_health_desc give a consumer a
// format-agnostic snapshot of one instance's health (bb_meminfo_heap_snap
// pattern) -- bb exposes RAW metrics; the consumer judges (no verdict here).
//
// Usage:
//   bb_tcp_client_cfg_t cfg = { .host = "stratum.example.com", .port = 3333 };
//   bb_tcp_client_t h;
//   bb_tcp_client_init("my_ns", &cfg, &h);  // or NULL cfg to load NVS-backed / Kconfig config
//   bb_tcp_client_connect(h);           // blocking, bounded by connect_timeout_ms
//   bb_tcp_client_write(h, buf, len);
//   bb_tcp_client_read(h, buf, sizeof(buf), &n);
//   bb_tcp_client_destroy(h);
#pragma once

#include "bb_core.h"
#include "bb_serialize.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_TCP_CLIENT_HOST_MAX 96

// Opaque handle — never dereference directly. Backed by a Kconfig-sized
// static instance pool (BB_TCP_CLIENT_MAX_INSTANCES); no heap allocation.
//
// The zero-heap guarantee above covers ONLY bb_tcp_client's own bookkeeping
// (the static pool + transport_health slot). On ESP-IDF, cfg.tls = true
// opens the connection through esp-tls/mbedTLS, which allocates several KB
// of heap internally per active TLS session — that allocation is NOT covered
// by this component's no-heap-by-default posture. Size no-PSRAM boards for
// concurrent TLS instances accordingly.
typedef void *bb_tcp_client_t;

typedef enum {
    BB_TCP_CLIENT_DISCONNECTED = 0,
    BB_TCP_CLIENT_CONNECTED    = 1,
} bb_tcp_client_state_t;

/**
 * Configuration for bb_tcp_client_init.
 *
 * host                — DNS name OR dotted-quad IPv4 literal (unlike
 *                        bb_udp_client, DNS resolution IS performed).
 * port                 — destination TCP port.
 * tls                  — open a TLS session instead of plain TCP.
 * connect_timeout_ms   — 0 -> Kconfig default (BB_TCP_CONNECT_TIMEOUT_MS).
 * io_timeout_ms        — 0 -> Kconfig default (BB_TCP_IO_TIMEOUT_MS); bounds
 *                         both read() and the short-write retry loop in
 *                         write().
 * ca_cert_pem          — server/CA cert PEM override; NULL => ESP cert
 *                         bundle (tls=true only).
 * client_cert_pem      — client cert PEM for mutual TLS (optional).
 * client_key_pem       — client private key PEM for mutual TLS (optional;
 *                         required together with client_cert_pem).
 *
 * Cert PEM pointers are NEVER persisted to NVS — bb_tcp_client_init(NULL, ..)
 * (the NVS-load path) never has certs; a TLS caller must always pass a
 * non-NULL cfg with the desired cert fields set.
 */
typedef struct {
    char        host[BB_TCP_CLIENT_HOST_MAX];
    uint16_t    port;
    bool        tls;
    uint32_t    connect_timeout_ms;
    uint32_t    io_timeout_ms;
    const char *ca_cert_pem;
    const char *client_cert_pem;
    const char *client_key_pem;
} bb_tcp_client_cfg_t;

/**
 * Acquire an instance from the static pool and load its configuration.
 * Does NOT connect — call bb_tcp_client_connect() separately.
 *
 * Pass cfg_or_null = NULL to load the persisted config (NVS namespace `ns`,
 * keys "host"/"port"/"tls"), falling back to Kconfig defaults for any unset
 * key. Pass non-NULL to override host/port/tls and persist them under `ns`;
 * connect_timeout_ms/io_timeout_ms/cert fields are used for this instance
 * only and are never persisted.
 *
 * @param ns           NVS namespace to load/persist host/port/tls under.
 *                      Required, borrowed (not copied — used only for the
 *                      duration of this call), ≤15 chars (NVS namespace
 *                      limit). NULL or empty ⇒ BB_ERR_INVALID_ARG (before any
 *                      storage is touched). The caller/composition decides
 *                      WHERE config lives; this component only declares WHAT
 *                      it stores.
 * @param cfg_or_null  Configuration, or NULL to load persisted/Kconfig defaults.
 * @param out          Receives the opaque handle on success.
 * @return BB_OK on success; BB_ERR_INVALID_ARG if ns is NULL/empty, out is
 *         NULL, or cfg_or_null is non-NULL and its host does not fit in
 *         BB_TCP_CLIENT_HOST_MAX - 1 chars; BB_ERR_NO_SPACE if the static
 *         instance pool (BB_TCP_CLIENT_MAX_INSTANCES) is exhausted.
 */
bb_err_t bb_tcp_client_init(const char *ns, const bb_tcp_client_cfg_t *cfg_or_null, bb_tcp_client_t *out);

/**
 * Establish the connection (blocking, bounded by cfg.connect_timeout_ms or
 * BB_TCP_CONNECT_TIMEOUT_MS). Idempotent: a no-op returning BB_OK if already
 * connected.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if h is NULL/invalid;
 *         BB_ERR_INVALID_STATE on connect failure (DNS, socket, TLS
 *         handshake). Updates this instance's health (connected/last_ok_ms
 *         on success; connected/fail_count on failure) -- see the header
 *         comment above.
 */
bb_err_t bb_tcp_client_connect(bb_tcp_client_t h);

/**
 * Read up to `len` bytes into `buf`. Not connected is a caller error, not a
 * transport failure.
 *
 * @return BB_OK with *out_len > 0 on data; BB_ERR_TIMEOUT with *out_len = 0
 *         if no data arrived within cfg.io_timeout_ms (does NOT update
 *         health — a timeout is not a failure signal); BB_ERR_INVALID_STATE
 *         if not connected; BB_ERR_INVALID_ARG for NULL h/buf/out_len; any
 *         other bb_err_t is a hard transport error (sets connected=false,
 *         bumps fail_count).
 */
bb_err_t bb_tcp_client_read(bb_tcp_client_t h, uint8_t *buf, size_t len, size_t *out_len);

/**
 * Write exactly `len` bytes of `buf`. Short writes are retried internally in
 * a loop bounded by cfg.io_timeout_ms.
 *
 * @return BB_OK once all `len` bytes are written; BB_ERR_INVALID_STATE if
 *         not connected; BB_ERR_INVALID_ARG for NULL h/buf; any other
 *         bb_err_t is a hard transport error (sets connected=false, bumps
 *         fail_count).
 */
bb_err_t bb_tcp_client_write(bb_tcp_client_t h, const uint8_t *buf, size_t len);

/**
 * Non-blocking-friendly readability seam: polls for up to `timeout_ms` and
 * reports whether a subsequent read() would return data, without itself
 * consuming any bytes. Lets a consumer FSM avoid stalling its task on
 * read().
 *
 * @return BB_OK with *out_readable set; BB_ERR_INVALID_STATE if not
 *         connected; BB_ERR_INVALID_ARG for NULL h/out_readable.
 */
bb_err_t bb_tcp_client_poll_readable(bb_tcp_client_t h, uint32_t timeout_ms, bool *out_readable);

/**
 * Close the connection. The handle itself stays valid — a later
 * bb_tcp_client_connect() call on the same handle must succeed (no state
 * poisoning). Safe to call when already disconnected.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if h is NULL/invalid.
 */
bb_err_t bb_tcp_client_close(bb_tcp_client_t h);

/** Returns the current connection state; BB_TCP_CLIENT_DISCONNECTED for a NULL/invalid handle. */
bb_tcp_client_state_t bb_tcp_client_get_state(bb_tcp_client_t h);

/**
 * Close (if connected) and release the instance back to the static pool.
 * Safe to call with NULL.
 */
bb_err_t bb_tcp_client_destroy(bb_tcp_client_t h);

// ---------------------------------------------------------------------------
// Per-instance health snapshot (B1-1039) — format-agnostic wire surface,
// mirroring bb_meminfo_heap_snap's descriptor+fill pattern. Every numeric
// field is widened to a fixed 64-bit width so bb_serialize_walk()'s
// BB_TYPE_I64/BB_TYPE_U64 cases (which always memcpy a fixed 8 bytes at the
// descriptor offset) never read past a narrower platform type.
// ---------------------------------------------------------------------------

typedef struct {
    bool     connected;
    int64_t  last_ok_ms;
    uint64_t fail_count;
    int64_t  tls_error_code;
} bb_tcp_client_health_snap_t;

extern const bb_serialize_desc_t bb_tcp_client_health_desc;

/**
 * Populates `out` from instance `h`'s live health state.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if h is NULL/invalid or out
 *         is NULL.
 */
bb_err_t bb_tcp_client_health_fill(bb_tcp_client_t h, bb_tcp_client_health_snap_t *out);

// ---------------------------------------------------------------------------
// Host test hooks (only when BB_TCP_CLIENT_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_TCP_CLIENT_TESTING

/** Reset every pooled instance and the host fake's captured state (test isolation). */
void bb_tcp_client_test_reset(void);

/**
 * Queue `len` bytes of `buf` for the next bb_tcp_client_read(h, ...) call(s)
 * to drain (possibly across multiple partial reads). Replaces any
 * not-yet-drained bytes previously injected for this handle.
 */
void bb_tcp_client_test_inject_readable(bb_tcp_client_t h, const uint8_t *buf, size_t len);

/**
 * Copy the most recent bb_tcp_client_write() payload for `h` into `out`
 * (capacity `out_cap`). Returns the captured length, or -1 if no write has
 * been captured yet or it does not fit in `out_cap`.
 */
int bb_tcp_client_test_last_write(bb_tcp_client_t h, uint8_t *out, size_t out_cap);

/** Number of bb_tcp_client_write() calls captured for `h` since init/reset. */
int bb_tcp_client_test_write_count(bb_tcp_client_t h);

/**
 * Force the result of the NEXT bb_tcp_client_connect(h) call to `err`
 * (BB_OK simulates a normal successful connect). One-shot: cleared after it
 * fires once.
 */
void bb_tcp_client_test_force_connect_result(bb_tcp_client_t h, bb_err_t err);

/**
 * Force the result of the NEXT bb_tcp_client_read(h, ...) or
 * bb_tcp_client_write(h, ...) call (whichever comes first) to `err`. Used to
 * simulate a hard transport error (any err other than BB_OK/BB_ERR_TIMEOUT
 * sets connected=false + bumps fail_count, matching the real backend).
 * One-shot.
 */
void bb_tcp_client_test_force_io_result(bb_tcp_client_t h, bb_err_t err);

/**
 * Force `h`'s health.tls_error_code directly, without going through a real
 * TLS failure path (the host backend has no real TLS). Lets a host test
 * exercise bb_tcp_client_health_fill()'s tls_error_code field.
 */
void bb_tcp_client_test_force_tls_error_code(bb_tcp_client_t h, int64_t tls_error_code);

/**
 * Sets the mock clock (BB_TCP_CLIENT_TESTING builds only) used for
 * last_ok_ms instead of bb_clock_now_ms64(), for deterministic host tests.
 * Reset to 0 by bb_tcp_client_test_reset().
 */
void bb_tcp_client_test_set_mock_time_ms64(int64_t ms);

#endif /* BB_TCP_CLIENT_TESTING */

#ifdef __cplusplus
}
#endif
