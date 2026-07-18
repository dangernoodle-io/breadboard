// bb_mqtt_client — per-instance health (B1-1040): the last_ok_ms clock
// source and the health snapshot descriptor, shared by the host and
// ESP-IDF backends (mirrors bb_tcp_client_common.c's B1-1039 pattern).
//
// Coherency: each backend's handle already carries its own lock (host: none
// needed -- single-writer-per-instance, single-threaded tests; espidf:
// h->lock, a SemaphoreHandle_t already used to guard get_stats()'s
// cross-task read). bb_mqtt_client_health_fill() reuses that existing lock
// in each backend rather than introducing a new one -- see the backends'
// bb_mqtt_client_health_fill() for the coherency comment.
#include "bb_mqtt_client_health.h"
#include "bb_clock.h"

#include <stddef.h>

#ifdef BB_MQTT_CLIENT_TESTING
static int64_t s_mock_time_ms64 = 0;

int64_t bb_mqtt_client_priv_now_ms64(void)
{
    return s_mock_time_ms64;
}

void bb_mqtt_client_test_set_mock_time_ms64(int64_t ms)
{
    s_mock_time_ms64 = ms;
}
#else
int64_t bb_mqtt_client_priv_now_ms64(void)
{
    return (int64_t)bb_clock_now_ms64();
}
#endif

void bb_mqtt_client_priv_health_close(bool *connected)
{
    *connected = false;  // clean close is not a transport failure -- fail_count untouched
}

// ---------------------------------------------------------------------------
// Health snapshot descriptor -- format-agnostic, portable (no ESP_PLATFORM
// gate needed). Mirrors bb_tcp_client_health_desc's structure/widening
// pattern; see bb_mqtt_client.h for the snapshot struct contract.
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_mqtt_client_health_fields[] = {
    { .key = "connected", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_mqtt_client_health_snap_t, connected) },
    { .key = "last_ok_ms", .type = BB_TYPE_I64,
      .offset = offsetof(bb_mqtt_client_health_snap_t, last_ok_ms) },
    { .key = "fail_count", .type = BB_TYPE_U64,
      .offset = offsetof(bb_mqtt_client_health_snap_t, fail_count) },
    { .key = "tls_error_code", .type = BB_TYPE_I64,
      .offset = offsetof(bb_mqtt_client_health_snap_t, tls_error_code) },
};

const bb_serialize_desc_t bb_mqtt_client_health_desc = {
    .type_name = "mqtt_client_health",
    .fields    = s_mqtt_client_health_fields,
    .n_fields  = sizeof(s_mqtt_client_health_fields) / sizeof(s_mqtt_client_health_fields[0]),
    .snap_size = sizeof(bb_mqtt_client_health_snap_t),
};
