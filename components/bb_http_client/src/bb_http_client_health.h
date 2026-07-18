// bb_http_client — per-session health internals (B1-1041). Not part of the
// public API; included via PRIV_INCLUDE_DIRS "src" from the host and
// ESP-IDF session backends only (platform/{host,espidf}/bb_http_client/*.c).
//
// SCOPE: health attaches to the keep-alive SESSION handle
// (bb_http_client_session_t) only -- the one-shot bb_http_client_get/_post
// calls have no persistent object to hang health on. See bb_http_client.h
// for the full reporting policy and the public snapshot/descriptor.
//
// Coherency: neither session backend struct (host_session_t,
// espidf_session_t) carries any other lock today, and
// bb_http_client_session_health_fill() is a cross-task diag/egress consumer
// BY DESIGN -- the owning task drives session_post()/session_close() while
// a different task reads health. Each backend embeds a
// bb_http_client_health_state_t (this struct) with its own pthread_mutex_t
// guarding JUST this sub-struct, mirroring bb_tcp_client_health_state_t's
// coherency rationale (see bb_tcp_client_priv.h, B1-1039).
#pragma once

#include "bb_http_client.h"
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool            connected;
    int64_t         last_ok_ms;
    uint32_t        fail_count;
    int64_t         tls_error_code;
    pthread_mutex_t lock;
} bb_http_client_health_state_t;

/**
 * Reports a session_post() outcome against `health` (see bb_http_client.h's
 * reporting policy comment for the full contract).
 *
 * transport_ok=true means the round trip completed and a response was
 * received (any HTTP status code) -- connected=true, last_ok_ms stamped.
 * status_code >= 500 ALSO bumps fail_count (a server error is a failure
 * signal even though the transport itself succeeded); a 4xx status leaves
 * fail_count untouched -- a valid server response, not a transport failure.
 *
 * transport_ok=false means the transport itself failed (DNS/TLS/socket, no
 * response) -- connected=false, fail_count bumped unconditionally
 * (status_code is ignored in this case).
 *
 * Takes health->lock for the duration of the field write.
 */
void bb_http_client_priv_health_report(bb_http_client_health_state_t *health,
                                       bool transport_ok, int status_code);

/**
 * Records a TLS-specific failure code without touching
 * connected/fail_count/last_ok_ms. Call whenever a session_post() result
 * carries a non-zero tls_error_code (success or failure path) -- never call
 * this to reset tls_error_code back to 0 on a later success (see
 * bb_http_client.h's reporting policy: "left at its last-reported value").
 * Takes health->lock for the duration of the field write.
 */
void bb_http_client_priv_health_set_tls_error(bb_http_client_health_state_t *health,
                                              int64_t tls_error_code);

/**
 * Clears connected (without touching fail_count) for a clean
 * session_close() -- see bb_http_client.h's reporting policy. Takes
 * health->lock for the duration of the field write.
 */
void bb_http_client_priv_health_close(bb_http_client_health_state_t *health);

/**
 * last_ok_ms clock source. bb_http_client is a networking client (not a
 * pure primitive), so it may read a clock directly -- bb_clock_now_ms64()
 * is the canonical source (see bb_clock.h). BB_HTTP_CLIENT_TESTING builds
 * (native host tests only) get a settable mock instead, keeping
 * last_ok_ms assertions deterministic (mirrors bb_tcp_client_priv.h /
 * bb_mqtt_client_health.h's identical seam).
 */
int64_t bb_http_client_priv_health_now_ms64(void);

#ifdef BB_HTTP_CLIENT_TESTING
/**
 * Sets the mock clock used for last_ok_ms instead of bb_clock_now_ms64(),
 * for deterministic host tests. Forwarded to test callers via
 * bb_http_client_host.h's bb_http_client_test_set_mock_time_ms64() (this
 * component's existing convention keeps host test hooks in the host port
 * header rather than the public bb_http_client.h). Not reset implicitly --
 * set it explicitly before an operation that stamps last_ok_ms.
 */
void bb_http_client_priv_health_set_mock_time_ms64(int64_t ms);
#endif

#ifdef __cplusplus
}
#endif
