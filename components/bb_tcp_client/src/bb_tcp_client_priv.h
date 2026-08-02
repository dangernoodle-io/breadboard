// bb_tcp_client — private helpers shared by the host and ESP-IDF backends.
// Not part of the public API; included via PRIV_INCLUDE_DIRS "src" from
// platform/{host,espidf}/bb_tcp_client/*.c only.
#pragma once

#include "bb_tcp_client.h"
#include "bb_tls_creds.h"
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Kconfig bridge (canonical two-step pattern; see bb_clock.h / bb_udp_client's
// bb_udp_client_priv.h). On ESP-IDF, Kconfig generates CONFIG_BB_TCP_* symbols.
// Bridge them to the resolved BB_TCP_* macros here so both backends (host,
// espidf) read one already-resolved definition instead of each re-deriving
// its own ad-hoc fallback. Never shadow the generated CONFIG_ symbol with a
// bare #ifndef.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_TCP_CLIENT_MAX_INSTANCES
#    define BB_TCP_CLIENT_MAX_INSTANCES CONFIG_BB_TCP_CLIENT_MAX_INSTANCES
#  endif
#  ifdef CONFIG_BB_TCP_CONNECT_TIMEOUT_MS
#    define BB_TCP_CONNECT_TIMEOUT_MS CONFIG_BB_TCP_CONNECT_TIMEOUT_MS
#  endif
#  ifdef CONFIG_BB_TCP_IO_TIMEOUT_MS
#    define BB_TCP_IO_TIMEOUT_MS CONFIG_BB_TCP_IO_TIMEOUT_MS
#  endif
#  ifdef CONFIG_BB_TCP_TLS_DEFAULT
#    define BB_TCP_TLS_DEFAULT CONFIG_BB_TCP_TLS_DEFAULT
#  endif
#endif

#ifndef BB_TCP_CLIENT_MAX_INSTANCES
#define BB_TCP_CLIENT_MAX_INSTANCES 1
#endif
#ifndef BB_TCP_CONNECT_TIMEOUT_MS
#define BB_TCP_CONNECT_TIMEOUT_MS 5000
#endif
#ifndef BB_TCP_IO_TIMEOUT_MS
#define BB_TCP_IO_TIMEOUT_MS 5000
#endif
#ifndef BB_TCP_TLS_DEFAULT
#define BB_TCP_TLS_DEFAULT 0
#endif

/**
 * Load bb_tcp_client_cfg_t host/port/tls from NVS namespace `ns`, falling
 * back to Kconfig defaults (host="", port=0, tls=BB_TCP_TLS_DEFAULT) for any
 * unset key. connect_timeout_ms/io_timeout_ms/cert fields are left zeroed —
 * cfg's caller-supplied-only fields never round-trip through NVS. `ns` is
 * borrowed — used only for the duration of this call.
 */
void bb_tcp_client_priv_load_from_nvs(const char *ns, bb_tcp_client_cfg_t *out);

/** Persist host/port/tls (only) to NVS namespace `ns` (borrowed). */
void bb_tcp_client_priv_save_to_nvs(const char *ns, const bb_tcp_client_cfg_t *cfg);

// ---------------------------------------------------------------------------
// TLS credential resolution (B1-1390). Portable (host + ESP-IDF) so the
// precedence logic below is host-testable; only the ESP-IDF backend actually
// wires the resolved creds into a real TLS session (bb_tcp_client's host
// backend has no real socket/TLS -- see platform/host/bb_tcp_client's
// header comment).
// ---------------------------------------------------------------------------

/**
 * Resolves TLS credentials for `cfg` via bb_tls_creds_resolve(), honoring
 * bb_tls_creds' own three-tier precedence: cfg's ca_cert_pem/client_cert_pem/
 * client_key_pem fields are passed through as the top-tier programmatic
 * override (non-NULL wins as-is); any left NULL fall through to an NVS
 * lookup in namespace `ns` (keys "tls_ca"/"tls_cert"/"tls_key") and then to
 * build-time embedded weak defaults. `ns` is borrowed, used only for the
 * duration of this call -- see bb_tcp_client.h's cfg doc.
 *
 * A no-op when cfg->tls is false: *out is zeroed and BB_OK is returned
 * without touching bb_tls_creds at all -- a plaintext instance must do no
 * credential work.
 *
 * @return BB_OK on success (including when tls is false, or when tls is true
 *         but nothing resolved anywhere -- an empty resolve is not an
 *         error, see bb_tls_creds_resolve()); BB_ERR_INVALID_ARG if cfg or
 *         out is NULL; BB_ERR_NO_SPACE on allocation failure inside
 *         bb_tls_creds_resolve().
 */
bb_err_t bb_tcp_client_priv_resolve_tls_creds(const char *ns, const bb_tcp_client_cfg_t *cfg,
                                               bb_tls_creds_t *out);

// ---------------------------------------------------------------------------
// Per-instance health (B1-1039). `bb_tcp_client_health_state_t` is embedded
// directly in each backend's pooled instance struct (host + espidf) -- no
// shared/authoritative transport-health slot. The report/set-tls-error
// helpers below live here (not per-backend) so the reporting policy (see
// bb_tcp_client.h) is defined in exactly one place.
//
// Coherency (firmware-review fix): connect/read/write/close() are the
// FSM/caller task's single-writer path, but bb_tcp_client_health_fill() is a
// cross-task diag/egress consumer BY DESIGN -- unlike the rest of an
// instance (single-writer-per-instance, no lock), the 4-field health
// snapshot IS read from a different task than the one driving the socket.
// `lock` guards JUST this sub-struct (not the whole instance) so a reader
// never observes a torn/inconsistent combination of the 64-bit
// last_ok_ms/tls_error_code fields on a 32-bit target. pthread_mutex_t is
// the SAME portable primitive bb_cache uses for its per-entry lock (see
// platform/espidf/bb_cache/bb_cache_espidf.c) -- portable across host and
// ESP-IDF (which ships pthread), and this report()/set_tls_error()/close()
// write frequency is per connection-result/transition, not a per-byte hot
// path, so a plain mutex is obviously-correct with negligible contention.
// Unlike bb_cache's process-lifetime mutex, this one is scoped to the
// instance's own acquire/release lifecycle: pthread_mutex_init() when a
// backend's bb_tcp_client_init() acquires the pooled slot,
// pthread_mutex_destroy() when bb_tcp_client_destroy() releases it back.
// ---------------------------------------------------------------------------

typedef struct {
    bool            connected;
    int64_t         last_ok_ms;
    uint32_t        fail_count;
    int64_t         tls_error_code;
    pthread_mutex_t lock;
} bb_tcp_client_health_state_t;

/**
 * Reports ok/fail against one instance's health state (see bb_tcp_client.h's
 * reporting policy). ok=true sets connected=true and stamps last_ok_ms;
 * ok=false sets connected=false and bumps fail_count. `health` is never
 * NULL -- both backends pass the address of their pooled instance's
 * embedded health member. Takes health->lock for the duration of the
 * field write (see the coherency comment above).
 */
void bb_tcp_client_priv_health_report(bb_tcp_client_health_state_t *health, bool ok);

/**
 * Records a TLS-specific failure code without touching
 * connected/fail_count/last_ok_ms. Called from the ESP-IDF backend's TLS
 * failure path; unused (never called) on the host backend, which has no
 * real TLS. Takes health->lock for the duration of the field write.
 */
void bb_tcp_client_priv_health_set_tls_error(bb_tcp_client_health_state_t *health, int64_t tls_error_code);

/**
 * Clears connected (without touching fail_count) for a clean close() -- see
 * bb_tcp_client.h's reporting policy. Takes health->lock for the duration of
 * the field write; both backends call this from close() instead of writing
 * health.connected directly, so the close-write is covered by the same
 * coherency guard as report()/set_tls_error().
 */
void bb_tcp_client_priv_health_close(bb_tcp_client_health_state_t *health);

#ifdef BB_TCP_CLIENT_TESTING
/** Resets the mock clock used for last_ok_ms (test isolation only). */
void bb_tcp_client_priv_reset_mock_clock_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
