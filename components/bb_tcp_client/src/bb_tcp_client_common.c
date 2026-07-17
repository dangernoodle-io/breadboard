// bb_tcp_client — common logic shared by the host and ESP-IDF backends:
// NVS-backed host/port/tls config load/save, the per-instance health
// report/set-tls-error helpers, and the health snapshot descriptor. The
// platform-specific files (platform/{host,espidf}/bb_tcp_client/*.c)
// implement the instance pool, the actual stream I/O, and
// bb_tcp_client_health_fill() (which needs each backend's own
// inst_from_handle()).
//
// host/port/tls round-trip through bb_config (typed layer over bb_storage)
// rather than bb_nv's generic KV forwarder (B1-756, bb_nv dissolution epic
// B1-708) — bb_config's STR/U16/U8 encodings resolve to the SAME
// nvs_set_str/nvs_set_u16/nvs_set_u8 calls bb_nv_set_str/set_u16/set_u8 made
// (both are thin forwarders to bb_storage_nvs, see bb_storage_nvs.h), so the
// on-flash key/type triples below are byte-compatible with what this
// component previously wrote via bb_nv (namespace is now caller-supplied,
// B1-951 — the component declares WHAT it stores, the composition decides
// WHERE; see bb_tls_creds's resolve_one for the reference pattern).
//
// B1-1039: health is now per-instance (see bb_tcp_client_priv.h); there is
// no shared/authoritative transport-health component in the loop.
#include "bb_tcp_client_priv.h"
#include "bb_config.h"
#include "bb_clock.h"

#include <stddef.h>
#include <string.h>

void bb_tcp_client_priv_load_from_nvs(const char *ns, bb_tcp_client_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

    // ns is caller-supplied at runtime, so these field descriptors are built
    // per-call rather than declared static const — bb_config_field_t carries
    // no state of its own, so a stack-local instance is safe to pass by
    // pointer for the duration of this single call (mirrors bb_tls_creds's
    // resolve_one).
    const bb_config_field_t host_field = {
        .id      = "tcp.host",
        .type    = BB_CONFIG_STR,
        .addr    = { .backend = "nvs", .ns_or_dir = ns, .key = "host" },
        .max_len = BB_TCP_CLIENT_HOST_MAX,
        .def     = { .str = "" },
        .has_default = true,
    };
    const bb_config_field_t port_field = {
        .id          = "tcp.port",
        .type        = BB_CONFIG_U16,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "port" },
        .def         = { .u16 = 0 },
        .has_default = true,
    };
    const bb_config_field_t tls_field = {
        .id          = "tcp.tls",
        .type        = BB_CONFIG_U8,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "tls" },
        .def         = { .u8 = BB_TCP_TLS_DEFAULT ? 1 : 0 },
        .has_default = true,
    };

    size_t out_len = 0;
    bb_config_get_str(&host_field, out->host, sizeof(out->host), &out_len);
    bb_config_get_u16(&port_field, &out->port);

    uint8_t tls_u8 = 0;
    bb_config_get_u8(&tls_field, &tls_u8);
    out->tls = (tls_u8 != 0);
}

void bb_tcp_client_priv_save_to_nvs(const char *ns, const bb_tcp_client_cfg_t *cfg)
{
    const bb_config_field_t host_field = {
        .id      = "tcp.host",
        .type    = BB_CONFIG_STR,
        .addr    = { .backend = "nvs", .ns_or_dir = ns, .key = "host" },
        .max_len = BB_TCP_CLIENT_HOST_MAX,
        .def     = { .str = "" },
        .has_default = true,
    };
    const bb_config_field_t port_field = {
        .id          = "tcp.port",
        .type        = BB_CONFIG_U16,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "port" },
        .def         = { .u16 = 0 },
        .has_default = true,
    };
    const bb_config_field_t tls_field = {
        .id          = "tcp.tls",
        .type        = BB_CONFIG_U8,
        .addr        = { .backend = "nvs", .ns_or_dir = ns, .key = "tls" },
        .def         = { .u8 = BB_TCP_TLS_DEFAULT ? 1 : 0 },
        .has_default = true,
    };

    bb_config_set_str(&host_field, cfg->host);
    bb_config_set_u16(&port_field, cfg->port);
    bb_config_set_u8(&tls_field, cfg->tls ? 1 : 0);
}

// ---------------------------------------------------------------------------
// last_ok_ms clock source. bb_tcp_client is a networking client (not a pure
// primitive), so it may read a clock directly -- bb_clock_now_ms64() is the
// canonical source (see bb_clock.h). BB_TCP_CLIENT_TESTING builds (native
// host tests only) get a settable mock instead, keeping last_ok_ms
// assertions deterministic -- mirrors bb_button_events' local mock-clock
// guard idiom (bb_clock.h: "components that need a settable mock define
// their own per-component mock guard").
// ---------------------------------------------------------------------------

#ifdef BB_TCP_CLIENT_TESTING
static int64_t s_mock_time_ms64 = 0;

static int64_t now_ms64(void)
{
    return s_mock_time_ms64;
}

void bb_tcp_client_test_set_mock_time_ms64(int64_t ms)
{
    s_mock_time_ms64 = ms;
}

void bb_tcp_client_priv_reset_mock_clock_for_test(void)
{
    s_mock_time_ms64 = 0;
}
#else
static int64_t now_ms64(void)
{
    return (int64_t)bb_clock_now_ms64();
}
#endif

void bb_tcp_client_priv_health_report(bb_tcp_client_health_state_t *health, bool ok)
{
    pthread_mutex_lock(&health->lock);
    if (ok) {
        health->connected  = true;
        health->last_ok_ms = now_ms64();
    } else {
        health->connected = false;
        health->fail_count++;
    }
    pthread_mutex_unlock(&health->lock);
}

void bb_tcp_client_priv_health_set_tls_error(bb_tcp_client_health_state_t *health, int64_t tls_error_code)
{
    pthread_mutex_lock(&health->lock);
    health->tls_error_code = tls_error_code;
    pthread_mutex_unlock(&health->lock);
}

void bb_tcp_client_priv_health_close(bb_tcp_client_health_state_t *health)
{
    pthread_mutex_lock(&health->lock);
    health->connected = false;  // clean close is not a transport failure -- fail_count untouched
    pthread_mutex_unlock(&health->lock);
}

// ---------------------------------------------------------------------------
// Health snapshot descriptor -- format-agnostic, portable (no ESP_PLATFORM
// gate needed). Mirrors bb_meminfo_heap_snap_desc's structure/widening
// pattern; see bb_tcp_client.h for the snapshot struct contract.
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_tcp_client_health_fields[] = {
    { .key = "connected", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_tcp_client_health_snap_t, connected) },
    { .key = "last_ok_ms", .type = BB_TYPE_I64,
      .offset = offsetof(bb_tcp_client_health_snap_t, last_ok_ms) },
    { .key = "fail_count", .type = BB_TYPE_U64,
      .offset = offsetof(bb_tcp_client_health_snap_t, fail_count) },
    { .key = "tls_error_code", .type = BB_TYPE_I64,
      .offset = offsetof(bb_tcp_client_health_snap_t, tls_error_code) },
};

const bb_serialize_desc_t bb_tcp_client_health_desc = {
    .type_name = "tcp_client_health",
    .fields    = s_tcp_client_health_fields,
    .n_fields  = sizeof(s_tcp_client_health_fields) / sizeof(s_tcp_client_health_fields[0]),
    .snap_size = sizeof(bb_tcp_client_health_snap_t),
};
