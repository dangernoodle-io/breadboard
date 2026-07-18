// bb_http_client — per-session health (B1-1041): the last_ok_ms clock
// source and the health snapshot descriptor, shared by the host and
// ESP-IDF session backends (mirrors bb_tcp_client_common.c's B1-1039
// pattern). See bb_http_client_health.h for the coherency rationale and
// bb_http_client.h for the public snapshot/descriptor + reporting policy.
#include "bb_http_client_health.h"
#include "bb_clock.h"

#include <stddef.h>

#ifdef BB_HTTP_CLIENT_TESTING
static int64_t s_mock_time_ms64 = 0;

int64_t bb_http_client_priv_health_now_ms64(void)
{
    return s_mock_time_ms64;
}

void bb_http_client_priv_health_set_mock_time_ms64(int64_t ms)
{
    s_mock_time_ms64 = ms;
}
#else
int64_t bb_http_client_priv_health_now_ms64(void)
{
    return (int64_t)bb_clock_now_ms64();
}
#endif

void bb_http_client_priv_health_report(bb_http_client_health_state_t *health,
                                       bool transport_ok, int status_code)
{
    pthread_mutex_lock(&health->lock);
    if (transport_ok) {
        health->connected  = true;
        health->last_ok_ms = bb_http_client_priv_health_now_ms64();
        if (status_code >= 500) {
            health->fail_count++;  // server error -- transport OK, response is a failure signal
        }
    } else {
        health->connected = false;
        health->fail_count++;
    }
    pthread_mutex_unlock(&health->lock);
}

void bb_http_client_priv_health_set_tls_error(bb_http_client_health_state_t *health,
                                              int64_t tls_error_code)
{
    pthread_mutex_lock(&health->lock);
    health->tls_error_code = tls_error_code;
    pthread_mutex_unlock(&health->lock);
}

void bb_http_client_priv_health_close(bb_http_client_health_state_t *health)
{
    pthread_mutex_lock(&health->lock);
    health->connected = false;  // clean close is not a transport failure -- fail_count untouched
    pthread_mutex_unlock(&health->lock);
}

// ---------------------------------------------------------------------------
// Health snapshot descriptor -- format-agnostic, portable (no ESP_PLATFORM
// gate needed). Mirrors bb_tcp_client_health_desc's structure/widening
// pattern; see bb_http_client.h for the snapshot struct contract.
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_http_client_session_health_fields[] = {
    { .key = "connected", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_http_client_session_health_snap_t, connected) },
    { .key = "last_ok_ms", .type = BB_TYPE_I64,
      .offset = offsetof(bb_http_client_session_health_snap_t, last_ok_ms) },
    { .key = "fail_count", .type = BB_TYPE_U64,
      .offset = offsetof(bb_http_client_session_health_snap_t, fail_count) },
    { .key = "tls_error_code", .type = BB_TYPE_I64,
      .offset = offsetof(bb_http_client_session_health_snap_t, tls_error_code) },
};

const bb_serialize_desc_t bb_http_client_session_health_desc = {
    .type_name = "http_client_session_health",
    .fields    = s_http_client_session_health_fields,
    .n_fields  = sizeof(s_http_client_session_health_fields) / sizeof(s_http_client_session_health_fields[0]),
    .snap_size = sizeof(bb_http_client_session_health_snap_t),
};
