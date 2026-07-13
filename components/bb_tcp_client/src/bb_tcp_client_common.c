// bb_tcp_client — common logic shared by the host and ESP-IDF backends:
// NVS-backed host/port/tls config load/save, and the shared
// bb_transport_health "tcp" registration/report. The platform-specific files
// (platform/{host,espidf}/bb_tcp_client/*.c) implement the instance pool and
// the actual stream I/O.
#include "bb_tcp_client_priv.h"
#include "bb_nv.h"
#include "bb_nv_namespaces.h"
#include "bb_once.h"
#include "bb_transport_health.h"

#include <string.h>
#ifdef BB_TCP_CLIENT_TESTING
#include <stdatomic.h>
#include <unistd.h>
#endif

void bb_tcp_client_priv_load_from_nvs(bb_tcp_client_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    bb_nv_get_str(BB_TCP_NVS_NS, "host", out->host, sizeof(out->host), "");
    bb_nv_get_u16(BB_TCP_NVS_NS, "port", &out->port, 0);

    uint8_t tls_u8 = 0;
    bb_nv_get_u8(BB_TCP_NVS_NS, "tls", &tls_u8, BB_TCP_TLS_DEFAULT ? 1 : 0);
    out->tls = (tls_u8 != 0);
}

void bb_tcp_client_priv_save_to_nvs(const bb_tcp_client_cfg_t *cfg)
{
    bb_nv_set_str(BB_TCP_NVS_NS, "host", cfg->host);
    bb_nv_set_u16(BB_TCP_NVS_NS, "port", cfg->port);
    bb_nv_set_u8(BB_TCP_NVS_NS, "tls", cfg->tls ? 1 : 0);
}

// Single shared AUTHORITATIVE slot for the whole component (not one per
// instance) — mirrors platform/host/bb_sink_mqtt.c's registration pattern.
// Registered lazily on the first report call. With
// BB_TCP_CLIENT_MAX_INSTANCES > 1, connect/read/write for DIFFERENT pooled
// instances can be driven concurrently from different tasks, so the lazy
// check-then-register below is a genuine race: bb_once guards it so exactly
// one caller ever runs bb_transport_health_register(), and every other
// (concurrent or later) caller blocks until that single registration has
// completed before reading s_th_handle.
static bb_transport_handle_t s_th_handle = BB_TRANSPORT_HANDLE_INVALID;
static bb_once_t s_th_once = BB_ONCE_INIT;

#ifdef BB_TCP_CLIENT_TESTING
// Test-only widening hook (structurally unreachable outside
// BB_TCP_CLIENT_TESTING builds — that symbol is defined only in
// platformio.ini's native test env build_flags, never in any
// CMakeLists/sdkconfig path). When enabled, register_health() below sleeps
// while it still holds bb_once's RUNNING state, deterministically forcing a
// concurrent second caller to observe RUNNING (not DONE) and actually enter
// bb_once_run's wait loop — mirrors test_bb_once.c's slow_incr idiom, which
// widens the guarded region instead of relying on scheduling luck.
static _Atomic bool s_th_register_delay_enabled = false;
#endif

static void bb_tcp_client_priv_register_health(void *ctx)
{
    (void)ctx;
#ifdef BB_TCP_CLIENT_TESTING
    if (atomic_load(&s_th_register_delay_enabled)) {
        usleep(20000); // hold RUNNING long enough for a racing caller to observe it
    }
#endif
    bb_transport_health_register("tcp", BB_TRANSPORT_AUTHORITATIVE, &s_th_handle);
}

void bb_tcp_client_priv_health_report(bool ok)
{
    bb_once_run(&s_th_once, bb_tcp_client_priv_register_health, NULL);
    bb_transport_health_report(s_th_handle, ok);
}

#ifdef BB_TCP_CLIENT_TESTING
void bb_tcp_client_priv_reset_health_for_test(void)
{
    s_th_handle = BB_TRANSPORT_HANDLE_INVALID;
    s_th_once = (bb_once_t)BB_ONCE_INIT;
    atomic_store(&s_th_register_delay_enabled, false);
}

void bb_tcp_client_test_set_register_delay(bool enabled)
{
    atomic_store(&s_th_register_delay_enabled, enabled);
}
#endif
