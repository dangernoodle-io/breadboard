// Tests for bb_tcp_client: portable connected TCP/TLS stream client (host
// stub — hermetic fake, no real socket; per bb_http_client's host-stub
// philosophy). Mirrors test_bb_udp_client.c's structure for the sibling
// datagram transport.
//
// B1-756 (bb_nv dissolution epic B1-708): host/port/tls now round-trip
// through bb_config (backend="nvs") instead of bb_nv's generic KV forwarder.
//
// B1-951: ns is now caller-supplied (no longer hardcoded "bb_tcp"), so
// reset_world() below uses a LOCAL, namespace-aware fake "nvs" backend
// instead of the shared test/test_host/fake_nvs_backend.h double -- that
// shared fake keys its store on addr->key ONLY and ignores addr->ns_or_dir
// entirely (fine for every OTHER consumer, which always addresses a single
// fixed namespace), which would silently collapse two different namespaces'
// "port" entries into the same slot and make the isolation test below
// meaningless. This local fake is scoped to this file only -- not a change
// to the shared double, which other components' tests still rely on.
#include "unity.h"
#include "bb_tcp_client.h"
#include "bb_transport_health.h"
#include "bb_config.h"
#include "bb_storage.h"
#include "bb_nv_namespaces.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

#define TCP_TEST_NVS_MAX_ENTRIES 8
#define TCP_TEST_NVS_MAX_VALUE   128
#define TCP_TEST_NVS_KEY_MAX     48  // "<ns>:<key>"

typedef struct {
    bool    used;
    char    key[TCP_TEST_NVS_KEY_MAX];
    size_t  len;
    uint8_t value[TCP_TEST_NVS_MAX_VALUE];
} tcp_test_nvs_entry_t;

static tcp_test_nvs_entry_t s_tcp_test_nvs[TCP_TEST_NVS_MAX_ENTRIES];

static void tcp_test_nvs_reset(void)
{
    memset(s_tcp_test_nvs, 0, sizeof(s_tcp_test_nvs));
}

static void tcp_test_nvs_compose_key(const bb_storage_addr_t *addr, char *out, size_t out_cap)
{
    snprintf(out, out_cap, "%s:%s", addr->ns_or_dir ? addr->ns_or_dir : "",
              addr->key ? addr->key : "");
}

static tcp_test_nvs_entry_t *tcp_test_nvs_find(const char *composed)
{
    for (int i = 0; i < TCP_TEST_NVS_MAX_ENTRIES; i++) {
        if (s_tcp_test_nvs[i].used && strcmp(s_tcp_test_nvs[i].key, composed) == 0) {
            return &s_tcp_test_nvs[i];
        }
    }
    return NULL;
}

static bb_err_t tcp_test_nvs_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap,
                                  size_t *out_len)
{
    (void)impl;
    char composed[TCP_TEST_NVS_KEY_MAX];
    tcp_test_nvs_compose_key(addr, composed, sizeof(composed));

    tcp_test_nvs_entry_t *e = tcp_test_nvs_find(composed);
    if (e == NULL) return BB_ERR_NOT_FOUND;
    *out_len = e->len;
    if (cap > 0) {
        size_t copy_len = e->len < cap ? e->len : cap;
        memcpy(buf, e->value, copy_len);
    }
    return BB_OK;
}

static bb_err_t tcp_test_nvs_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl;
    if (len > TCP_TEST_NVS_MAX_VALUE) return BB_ERR_NO_SPACE;

    char composed[TCP_TEST_NVS_KEY_MAX];
    tcp_test_nvs_compose_key(addr, composed, sizeof(composed));

    tcp_test_nvs_entry_t *e = tcp_test_nvs_find(composed);
    if (e == NULL) {
        for (int i = 0; i < TCP_TEST_NVS_MAX_ENTRIES; i++) {
            if (!s_tcp_test_nvs[i].used) { e = &s_tcp_test_nvs[i]; break; }
        }
        if (e == NULL) return BB_ERR_NO_SPACE;
        strncpy(e->key, composed, sizeof(e->key) - 1);
        e->key[sizeof(e->key) - 1] = '\0';
        e->used = true;
    }
    if (len > 0) memcpy(e->value, buf, len);
    e->len = len;
    return BB_OK;
}

static bb_err_t tcp_test_nvs_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    char composed[TCP_TEST_NVS_KEY_MAX];
    tcp_test_nvs_compose_key(addr, composed, sizeof(composed));

    tcp_test_nvs_entry_t *e = tcp_test_nvs_find(composed);
    if (e != NULL) memset(e, 0, sizeof(*e));
    return BB_OK;
}

static bool tcp_test_nvs_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    char composed[TCP_TEST_NVS_KEY_MAX];
    tcp_test_nvs_compose_key(addr, composed, sizeof(composed));
    return tcp_test_nvs_find(composed) != NULL;
}

static const bb_storage_vtable_t s_tcp_test_nvs_vtable = {
    .get    = tcp_test_nvs_get,
    .set    = tcp_test_nvs_set,
    .erase  = tcp_test_nvs_erase,
    .exists = tcp_test_nvs_exists,
};

// Test-local field descriptors targeting the EXACT SAME address (backend/ns/
// key/type) as bb_tcp_client_common.c's internal per-call host_field/
// port_field/tls_field -- a literal-address BITE test proving
// bb_tcp_client_priv_save_to_nvs/_load_from_nvs actually land on that
// address, not merely that init() returns BB_OK.
static const bb_config_field_t s_test_tcp_host_field = {
    .id   = "test.tcp.host", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = BB_TCP_NVS_NS, .key = "host" },
};
static const bb_config_field_t s_test_tcp_port_field = {
    .id   = "test.tcp.port", .type = BB_CONFIG_U16,
    .addr = { .backend = "nvs", .ns_or_dir = BB_TCP_NVS_NS, .key = "port" },
};
static const bb_config_field_t s_test_tcp_tls_field = {
    .id   = "test.tcp.tls", .type = BB_CONFIG_U8,
    .addr = { .backend = "nvs", .ns_or_dir = BB_TCP_NVS_NS, .key = "tls" },
};

static void reset_world(void)
{
    bb_tcp_client_test_reset();
    bb_transport_health_reset_for_test();
    bb_storage_test_reset();
    tcp_test_nvs_reset();
    bb_storage_register_backend("nvs", &s_tcp_test_nvs_vtable, NULL);
}

static bb_tcp_client_t make_instance(const bb_tcp_client_cfg_t *cfg)
{
    bb_tcp_client_t h = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_init(BB_TCP_NVS_NS, cfg, &h));
    TEST_ASSERT_NOT_NULL(h);
    return h;
}

// ---------------------------------------------------------------------------
// init: NULL vs cfg (NVS load/persist)
// ---------------------------------------------------------------------------

void test_bb_tcp_client_init_null_loads_defaults(void)
{
    reset_world();

    bb_tcp_client_t h = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_init(BB_TCP_NVS_NS, NULL, &h));
    TEST_ASSERT_NOT_NULL(h);
}

void test_bb_tcp_client_init_persists_and_reloads(void)
{
    reset_world();

    bb_tcp_client_cfg_t cfg = { .host = "stratum.example.com", .port = 3333, .tls = true };
    bb_tcp_client_t h1 = make_instance(&cfg);
    bb_tcp_client_destroy(h1);

    // BITE: the literal "bb_tcp"/host,port,tls address (the SAME one bb_nv
    // used) must hold the persisted values -- proves the storage write
    // actually landed at the byte-compat address, not just that init()
    // returned BB_OK.
    char host_out[BB_TCP_CLIENT_HOST_MAX] = {0};
    size_t host_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&s_test_tcp_host_field, host_out, sizeof(host_out), &host_len));
    TEST_ASSERT_EQUAL_STRING("stratum.example.com", host_out);

    uint16_t port_out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u16(&s_test_tcp_port_field, &port_out));
    TEST_ASSERT_EQUAL_UINT16(3333, port_out);

    uint8_t tls_out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u8(&s_test_tcp_tls_field, &tls_out));
    TEST_ASSERT_EQUAL_UINT8(1, tls_out);

    // Re-init with NULL must reload the persisted host/port/tls values.
    bb_tcp_client_t h2 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_init(BB_TCP_NVS_NS, NULL, &h2));
    TEST_ASSERT_NOT_NULL(h2);
}

void test_bb_tcp_client_init_null_out_returns_invalid_arg(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_init(BB_TCP_NVS_NS, NULL, NULL));
}

// host overflow -> BB_ERR_INVALID_ARG
void test_bb_tcp_client_init_host_too_long_returns_invalid_arg(void)
{
    reset_world();

    bb_tcp_client_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    // Fill to full capacity — no room left for a NUL terminator.
    memset(cfg.host, 'a', sizeof(cfg.host));
    cfg.port = 3333;

    bb_tcp_client_t h = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_init(BB_TCP_NVS_NS, &cfg, &h));
}

// ---------------------------------------------------------------------------
// init: ns is required — NULL or empty ns -> BB_ERR_INVALID_ARG, no storage
// touched, no pool slot consumed (B1-951).
// ---------------------------------------------------------------------------

void test_bb_tcp_client_init_null_ns_returns_invalid_arg(void)
{
    reset_world();

    bb_tcp_client_t h = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_init(NULL, NULL, &h));
    TEST_ASSERT_NULL(h);

    // No pool slot consumed: a subsequent valid init must still succeed on
    // instance 0 (TEST_MAX_INSTANCES-worth of room untouched).
    bb_tcp_client_t h2 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_init(BB_TCP_NVS_NS, NULL, &h2));
    TEST_ASSERT_NOT_NULL(h2);
}

void test_bb_tcp_client_init_empty_ns_returns_invalid_arg(void)
{
    reset_world();

    bb_tcp_client_t h = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_init("", NULL, &h));
    TEST_ASSERT_NULL(h);
}

// ---------------------------------------------------------------------------
// two-namespace isolation: init/save under one ns must not be visible under
// a different ns. This is the entire point of B1-951 -- before this change
// the namespace was hardcoded, so this scenario had no way to exist.
//
// BITE PROOF (recorded here, not just asserted): with
// bb_tcp_client_priv_save_to_nvs/_load_from_nvs's `ns` parameter temporarily
// ignored in favor of a hardcoded literal (mirroring the pre-B1-951
// behavior), this test goes RED --
//
//   test_bb_tcp_client_init_two_namespaces_are_isolated:684:FAIL: Expected 0
//   Was 4242
//
// (host under "ns_b" comes back populated with the "ns_a" instance's
// persisted port instead of the "ns_b" default) -- proving the isolation is
// real, not an artifact of the test only checking a return code.
// ---------------------------------------------------------------------------

void test_bb_tcp_client_init_two_namespaces_are_isolated(void)
{
    reset_world();

    const bb_config_field_t ns_a_port_field = {
        .id = "test.ns_a.tcp.port", .type = BB_CONFIG_U16,
        .addr = { .backend = "nvs", .ns_or_dir = "ns_a", .key = "port" },
    };
    const bb_config_field_t ns_b_port_field = {
        .id = "test.ns_b.tcp.port", .type = BB_CONFIG_U16,
        .addr = { .backend = "nvs", .ns_or_dir = "ns_b", .key = "port" },
    };

    bb_tcp_client_cfg_t cfg_a = { .host = "a.example.com", .port = 4242, .tls = false };
    bb_tcp_client_t h_a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_init("ns_a", &cfg_a, &h_a));
    bb_tcp_client_destroy(h_a);

    // "ns_a" holds the value written under it.
    uint16_t port_a = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u16(&ns_a_port_field, &port_a));
    TEST_ASSERT_EQUAL_UINT16(4242, port_a);

    // "ns_b" has never been written -- must NOT see "ns_a"'s value. Reading
    // through init(NULL cfg) under "ns_b" must fall back to the Kconfig
    // default (port=0), not "ns_a"'s persisted 4242.
    bb_tcp_client_t h_b = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_init("ns_b", NULL, &h_b));
    bb_tcp_client_destroy(h_b);

    // Direct proof (no default-fallback masking): the "ns_b" storage
    // address genuinely has no entry -- "ns_a"'s write did not bleed
    // through. ns_b_port_field carries no has_default, so a bled-through
    // 4242 (or any stored value) would return BB_OK here, not NOT_FOUND.
    uint16_t port_b = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_config_get_u16(&ns_b_port_field, &port_b));

    // Now write a DIFFERENT value under "ns_b" and prove "ns_a" is untouched.
    bb_tcp_client_cfg_t cfg_b = { .host = "b.example.com", .port = 9999, .tls = false };
    bb_tcp_client_t h_b2 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_init("ns_b", &cfg_b, &h_b2));
    bb_tcp_client_destroy(h_b2);

    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u16(&ns_b_port_field, &port_b));
    TEST_ASSERT_EQUAL_UINT16(9999, port_b);

    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u16(&ns_a_port_field, &port_a));
    TEST_ASSERT_EQUAL_UINT16(4242, port_a);  // unchanged by the "ns_b" write
}

// instance-pool exhaustion -> BB_ERR_NO_SPACE
//
// Host test env builds with BB_TCP_CLIENT_MAX_INSTANCES=2 (platformio.ini) —
// the literal 2 below matches that override, not the Kconfig default (1), so
// this test also proves the free-slot scan is genuinely N-ready.
#define TEST_MAX_INSTANCES 2

void test_bb_tcp_client_init_pool_exhausted_returns_no_space(void)
{
    reset_world();

    bb_tcp_client_t handles[TEST_MAX_INSTANCES];
    for (int i = 0; i < TEST_MAX_INSTANCES; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_init(BB_TCP_NVS_NS, NULL, &handles[i]));
    }

    bb_tcp_client_t overflow = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_tcp_client_init(BB_TCP_NVS_NS, NULL, &overflow));

    for (int i = 0; i < TEST_MAX_INSTANCES; i++) {
        bb_tcp_client_destroy(handles[i]);
    }
}

// TLS cfg fields captured intact (not overwritten/dropped by init)
void test_bb_tcp_client_init_tls_cfg_captured(void)
{
    reset_world();

    bb_tcp_client_cfg_t cfg = {
        .host = "broker.example.com",
        .port = 8883,
        .tls = true,
        .connect_timeout_ms = 1234,
        .io_timeout_ms = 5678,
        .ca_cert_pem = "-----BEGIN CERTIFICATE-----\nfake\n-----END CERTIFICATE-----\n",
        .client_cert_pem = "-----BEGIN CERTIFICATE-----\nclient\n-----END CERTIFICATE-----\n",
        .client_key_pem = "-----BEGIN PRIVATE KEY-----\nkey\n-----END PRIVATE KEY-----\n",
    };
    bb_tcp_client_t h = make_instance(&cfg);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));
    TEST_ASSERT_EQUAL(BB_TCP_CLIENT_CONNECTED, bb_tcp_client_get_state(h));
}

// ---------------------------------------------------------------------------
// connect
// ---------------------------------------------------------------------------

void test_bb_tcp_client_connect_happy_sets_connected_and_reports_health_ok(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);

    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));
    TEST_ASSERT_EQUAL(BB_TCP_CLIENT_CONNECTED, bb_tcp_client_get_state(h));

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(1, enabled);
    TEST_ASSERT_EQUAL_INT(0, failing);
}

void test_bb_tcp_client_connect_is_idempotent_when_already_connected(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);

    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));
    TEST_ASSERT_EQUAL(BB_TCP_CLIENT_CONNECTED, bb_tcp_client_get_state(h));
}

void test_bb_tcp_client_connect_forced_fail_sets_disconnected_and_reports_health_fail(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);

    bb_tcp_client_test_force_connect_result(h, BB_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_tcp_client_connect(h));
    TEST_ASSERT_EQUAL(BB_TCP_CLIENT_DISCONNECTED, bb_tcp_client_get_state(h));

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(1, enabled);
    TEST_ASSERT_EQUAL_INT(1, failing);
}

void test_bb_tcp_client_connect_null_handle_returns_invalid_arg(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_connect(NULL));
}

// ---------------------------------------------------------------------------
// read/write before connect -> INVALID_STATE
// ---------------------------------------------------------------------------

void test_bb_tcp_client_read_before_connect_returns_invalid_state(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);

    uint8_t buf[8];
    size_t n = 99;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_tcp_client_read(h, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_INT(0, n);
}

void test_bb_tcp_client_write_before_connect_returns_invalid_state(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);

    const uint8_t buf[] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_tcp_client_write(h, buf, sizeof(buf)));
}

void test_bb_tcp_client_read_write_null_args_return_invalid_arg(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    uint8_t buf[8];
    size_t n;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_read(h, NULL, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_read(h, buf, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_write(h, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_read(NULL, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_write(NULL, buf, sizeof(buf)));
}

// ---------------------------------------------------------------------------
// read returns injected bytes, including partial reads
// ---------------------------------------------------------------------------

void test_bb_tcp_client_read_returns_injected_bytes(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    bb_tcp_client_test_inject_readable(h, payload, sizeof(payload));

    uint8_t out[16];
    size_t n = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_read(h, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_INT(sizeof(payload), n);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, sizeof(payload));
}

void test_bb_tcp_client_read_partial_drains_across_multiple_calls(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    const uint8_t payload[] = { 1, 2, 3, 4, 5 };
    bb_tcp_client_test_inject_readable(h, payload, sizeof(payload));

    uint8_t out[3];
    size_t n = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_read(h, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, 3);

    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_read(h, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_MEMORY(payload + 3, out, 2);
}

// read timeout -> BB_ERR_TIMEOUT + out_len 0, and no health report
void test_bb_tcp_client_read_timeout_reports_no_health(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    int enabled_before = -1, failing_before = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled_before, &failing_before));

    uint8_t out[8];
    size_t n = 99;
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_tcp_client_read(h, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_INT(0, n);

    int enabled_after = -1, failing_after = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled_after, &failing_after));
    TEST_ASSERT_EQUAL_INT(failing_before, failing_after);
}

// ---------------------------------------------------------------------------
// write is captured
// ---------------------------------------------------------------------------

void test_bb_tcp_client_write_is_captured(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    const uint8_t payload[] = { 7, 7, 7 };
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_write(h, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(1, bb_tcp_client_test_write_count(h));

    uint8_t out[16];
    int n = bb_tcp_client_test_last_write(h, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(sizeof(payload), n);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, sizeof(payload));
}

void test_bb_tcp_client_write_count_increments_across_writes(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    const uint8_t a[] = { 1 };
    const uint8_t b[] = { 2, 2 };
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_write(h, a, sizeof(a)));
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_write(h, b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(2, bb_tcp_client_test_write_count(h));

    uint8_t out[16];
    int n = bb_tcp_client_test_last_write(h, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(sizeof(b), n);
    TEST_ASSERT_EQUAL_MEMORY(b, out, sizeof(b));
}

void test_bb_tcp_client_last_write_before_any_write_returns_negative(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    uint8_t out[16];
    TEST_ASSERT_EQUAL_INT(-1, bb_tcp_client_test_last_write(h, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, bb_tcp_client_test_write_count(h));
}

void test_bb_tcp_client_last_write_too_small_cap_returns_negative(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    const uint8_t payload[] = { 1, 2, 3, 4 };
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_write(h, payload, sizeof(payload)));

    uint8_t tiny[2];
    TEST_ASSERT_EQUAL_INT(-1, bb_tcp_client_test_last_write(h, tiny, sizeof(tiny)));
}

// ---------------------------------------------------------------------------
// hard-error on read/write -> health ok=false
// ---------------------------------------------------------------------------

void test_bb_tcp_client_read_hard_error_reports_health_fail(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));  // seeds health ok=true

    bb_tcp_client_test_force_io_result(h, BB_ERR_INVALID_STATE);
    uint8_t buf[8];
    size_t n = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_tcp_client_read(h, buf, sizeof(buf), &n));

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(1, failing);
}

void test_bb_tcp_client_write_hard_error_reports_health_fail(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));  // seeds health ok=true

    bb_tcp_client_test_force_io_result(h, BB_ERR_INVALID_STATE);
    const uint8_t buf[] = { 1 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_tcp_client_write(h, buf, sizeof(buf)));

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(1, failing);
}

// forced io result of BB_ERR_TIMEOUT must NOT report health (mirrors the
// real-backend "plain timeout" carve-out) — covers the rc==BB_ERR_TIMEOUT
// branch of the forced-io-result path in both read() and write().
void test_bb_tcp_client_forced_io_timeout_does_not_report_health(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));  // seeds health ok=true

    bb_tcp_client_test_force_io_result(h, BB_ERR_TIMEOUT);
    uint8_t buf[8];
    size_t n = 0;
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_tcp_client_read(h, buf, sizeof(buf), &n));

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(0, failing);

    bb_tcp_client_test_force_io_result(h, BB_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_tcp_client_write(h, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(0, failing);
}

// ---------------------------------------------------------------------------
// poll_readable
// ---------------------------------------------------------------------------

void test_bb_tcp_client_poll_readable_true_when_data_queued(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    const uint8_t payload[] = { 1 };
    bb_tcp_client_test_inject_readable(h, payload, sizeof(payload));

    bool readable = false;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_poll_readable(h, 100, &readable));
    TEST_ASSERT_TRUE(readable);
}

void test_bb_tcp_client_poll_readable_false_when_no_data(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    bool readable = true;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_poll_readable(h, 100, &readable));
    TEST_ASSERT_FALSE(readable);
}

void test_bb_tcp_client_poll_readable_before_connect_returns_invalid_state(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);

    bool readable = true;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_tcp_client_poll_readable(h, 100, &readable));
    TEST_ASSERT_FALSE(readable);
}

void test_bb_tcp_client_poll_readable_null_args_return_invalid_arg(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_poll_readable(h, 100, NULL));
    bool readable = false;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_poll_readable(NULL, 100, &readable));
}

// ---------------------------------------------------------------------------
// close: no state poisoning — reconnect on same handle succeeds
// ---------------------------------------------------------------------------

void test_bb_tcp_client_close_then_read_write_return_invalid_state(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_close(h));
    TEST_ASSERT_EQUAL(BB_TCP_CLIENT_DISCONNECTED, bb_tcp_client_get_state(h));

    uint8_t buf[4];
    size_t n = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_tcp_client_read(h, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_tcp_client_write(h, buf, sizeof(buf)));
}

void test_bb_tcp_client_close_then_reconnect_succeeds(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_close(h));

    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));
    TEST_ASSERT_EQUAL(BB_TCP_CLIENT_CONNECTED, bb_tcp_client_get_state(h));
}

void test_bb_tcp_client_close_when_already_disconnected_is_safe(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_close(h));
    TEST_ASSERT_EQUAL(BB_TCP_CLIENT_DISCONNECTED, bb_tcp_client_get_state(h));
}

void test_bb_tcp_client_close_null_handle_returns_invalid_arg(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_tcp_client_close(NULL));
}

// ---------------------------------------------------------------------------
// get_state
// ---------------------------------------------------------------------------

void test_bb_tcp_client_get_state_null_handle_returns_disconnected(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_TCP_CLIENT_DISCONNECTED, bb_tcp_client_get_state(NULL));
}

// ---------------------------------------------------------------------------
// destroy
// ---------------------------------------------------------------------------

void test_bb_tcp_client_destroy_on_connected_handle_frees_pool_slot(void)
{
    reset_world();
    bb_tcp_client_t h = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h));

    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_destroy(h));

    // Pool slot must be free again — no leaked slot after destroy.
    bb_tcp_client_t h2 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_init(BB_TCP_NVS_NS, NULL, &h2));
    TEST_ASSERT_NOT_NULL(h2);
}

void test_bb_tcp_client_destroy_null_is_noop(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_destroy(NULL));
}

// ---------------------------------------------------------------------------
// transport_health registered exactly once across repeated init
// ---------------------------------------------------------------------------

void test_bb_tcp_client_transport_health_registered_once_across_repeated_init(void)
{
    reset_world();

    bb_tcp_client_t h1 = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h1));
    bb_tcp_client_destroy(h1);

    bb_tcp_client_t h2 = make_instance(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_tcp_client_connect(h2));
    bb_tcp_client_destroy(h2);

    bb_tcp_client_t h3 = make_instance(NULL);
    bb_tcp_client_test_force_connect_result(h3, BB_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_tcp_client_connect(h3));
    bb_tcp_client_destroy(h3);

    // Exactly one AUTHORITATIVE "tcp" slot exists no matter how many times
    // init/connect/destroy cycled — never re-registered per instance.
    bb_transport_health_snapshot_t snaps[8];
    size_t count = bb_transport_health_snapshot_all(snaps, 8);
    int tcp_slots = 0;
    for (size_t i = 0; i < count; i++) {
        if (snaps[i].name != NULL && strcmp(snaps[i].name, "tcp") == 0) {
            tcp_slots++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, tcp_slots);
}

// ---------------------------------------------------------------------------
// concurrent lazy registration: two pooled instances, driven from two
// threads, both racing bb_tcp_client_priv_health_report()'s lazy
// check-then-register for the very first time. Without bb_once guarding it,
// two threads can both observe BB_TRANSPORT_HANDLE_INVALID and each register
// its own "tcp" AUTHORITATIVE slot.
//
// The ready/go gate below only makes both threads ENTER connect() together —
// on its own that is NOT enough to force the interleaving: without widening
// the guarded region, the winner's registration is fast enough that the
// loser's very first check almost always already observes DONE, so the
// pre-fix naive check-then-act bug reproduces standalone only ~3% of the
// time. bb_tcp_client_test_set_register_delay(true) closes that gap by
// making bb_tcp_client_priv_register_health() sleep ~20ms while still
// holding bb_once's RUNNING state (mirrors test_bb_once.c's
// slow_incr/test_bb_once_run_loser_waits_via_sched_yield idiom), guaranteeing
// the second thread arrives while the first is still inside the guarded
// region. This test proves the guard holds deterministically, not just "in
// this run": with the bb_once fix reverted to a naive check-then-act, it
// fails reliably; with the fix in place, it passes reliably.
// ---------------------------------------------------------------------------

#define TCP_RACE_THREADS 2

static _Atomic int  s_race_ready_count;
static _Atomic bool s_race_go;

static void *bb_tcp_client_test_race_worker(void *arg)
{
    bb_tcp_client_t h = (bb_tcp_client_t)arg;

    atomic_fetch_add(&s_race_ready_count, 1);
    while (!atomic_load(&s_race_go)) {
        sched_yield();
    }

    bb_tcp_client_connect(h);
    return NULL;
}

void test_bb_tcp_client_concurrent_first_connect_registers_health_exactly_once(void)
{
    reset_world();
    atomic_store(&s_race_ready_count, 0);
    atomic_store(&s_race_go, false);
    bb_tcp_client_test_set_register_delay(true);

    bb_tcp_client_t handles[TCP_RACE_THREADS];
    for (int i = 0; i < TCP_RACE_THREADS; i++) {
        handles[i] = make_instance(NULL);
    }

    pthread_t threads[TCP_RACE_THREADS];
    for (int i = 0; i < TCP_RACE_THREADS; i++) {
        int rc = pthread_create(&threads[i], NULL, bb_tcp_client_test_race_worker, handles[i]);
        TEST_ASSERT_EQUAL_INT(0, rc);
    }

    // Release both threads together only once both have reached the gate,
    // so they race bb_tcp_client_priv_health_report()'s lazy registration
    // as close to simultaneously as the scheduler allows. The widened
    // guarded region (above) then guarantees the loser actually observes
    // RUNNING rather than DONE.
    while (atomic_load(&s_race_ready_count) < TCP_RACE_THREADS) {
        sched_yield();
    }
    atomic_store(&s_race_go, true);

    for (int i = 0; i < TCP_RACE_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    bb_tcp_client_test_set_register_delay(false);

    bb_transport_health_snapshot_t snaps[8];
    size_t count = bb_transport_health_snapshot_all(snaps, 8);
    int tcp_slots = 0;
    for (size_t i = 0; i < count; i++) {
        if (snaps[i].name != NULL && strcmp(snaps[i].name, "tcp") == 0) {
            tcp_slots++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, tcp_slots);

    for (int i = 0; i < TCP_RACE_THREADS; i++) {
        bb_tcp_client_destroy(handles[i]);
    }
}
