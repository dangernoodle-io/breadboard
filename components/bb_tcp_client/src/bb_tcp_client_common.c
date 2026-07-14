// bb_tcp_client — common logic shared by the host and ESP-IDF backends:
// NVS-backed host/port/tls config load/save, and the shared
// bb_transport_health "tcp" registration/report. The platform-specific files
// (platform/{host,espidf}/bb_tcp_client/*.c) implement the instance pool and
// the actual stream I/O.
//
// host/port/tls round-trip through bb_config (typed layer over bb_storage)
// rather than bb_nv's generic KV forwarder (B1-756, bb_nv dissolution epic
// B1-708) — bb_config's STR/U16/U8 encodings resolve to the SAME
// nvs_set_str/nvs_set_u16/nvs_set_u8 calls bb_nv_set_str/set_u16/set_u8 made
// (both are thin forwarders to bb_storage_nvs, see bb_storage_nvs.h), so the
// on-flash namespace/key/type triples below are byte-compatible with what
// this component previously wrote via bb_nv.
#include "bb_tcp_client_priv.h"
#include "bb_config.h"
#include "bb_nv_namespaces.h"
#include "bb_once.h"
#include "bb_transport_health.h"

#include <string.h>
#ifdef BB_TCP_CLIENT_TESTING
#include <stdatomic.h>
#include <unistd.h>
#endif

// Namespace/keys byte-for-byte matched to bb_nv's prior BB_TCP_NVS_NS
// ("bb_tcp")/"host"/"port"/"tls" — do not change without a migration plan,
// this strands provisioned-board TCP transport config otherwise.
static const bb_config_field_t s_tcp_host_field = {
    .id      = "tcp.host",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "nvs", .ns_or_dir = BB_TCP_NVS_NS, .key = "host" },
    .max_len = BB_TCP_CLIENT_HOST_MAX,
    .def     = { .str = "" },
    .has_default = true,
};

static const bb_config_field_t s_tcp_port_field = {
    .id          = "tcp.port",
    .type        = BB_CONFIG_U16,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_TCP_NVS_NS, .key = "port" },
    .def         = { .u16 = 0 },
    .has_default = true,
};

static const bb_config_field_t s_tcp_tls_field = {
    .id          = "tcp.tls",
    .type        = BB_CONFIG_U8,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_TCP_NVS_NS, .key = "tls" },
    .def         = { .u8 = BB_TCP_TLS_DEFAULT ? 1 : 0 },
    .has_default = true,
};

void bb_tcp_client_priv_load_from_nvs(bb_tcp_client_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

    size_t out_len = 0;
    bb_config_get_str(&s_tcp_host_field, out->host, sizeof(out->host), &out_len);
    bb_config_get_u16(&s_tcp_port_field, &out->port);

    uint8_t tls_u8 = 0;
    bb_config_get_u8(&s_tcp_tls_field, &tls_u8);
    out->tls = (tls_u8 != 0);
}

void bb_tcp_client_priv_save_to_nvs(const bb_tcp_client_cfg_t *cfg)
{
    bb_config_set_str(&s_tcp_host_field, cfg->host);
    bb_config_set_u16(&s_tcp_port_field, cfg->port);
    bb_config_set_u8(&s_tcp_tls_field, cfg->tls ? 1 : 0);
}

// Single shared AUTHORITATIVE slot for the whole component (not one per
// instance). Registered lazily on the first report call. With
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
