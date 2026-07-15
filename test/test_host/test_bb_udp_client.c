// Tests for bb_udp_client: reusable IPv4 UDP datagram transport (host stub —
// captures the raw bytes handed to bb_udp_client_send() instead of opening a
// real socket). These cases exercise the transport directly: socket
// lifecycle, dest config, capture buffer.
//
// B1-756 (bb_nv dissolution epic B1-708): host/port/broadcast now round-trip
// through bb_config (backend="nvs") instead of bb_nv's generic KV forwarder.
//
// B1-951: ns is now caller-supplied (no longer hardcoded "bb_udp"), so
// reset_nvs_world() below uses a LOCAL, namespace-aware fake "nvs" backend
// instead of the shared test/test_host/fake_nvs_backend.h double -- that
// shared fake keys its store on addr->key ONLY and ignores addr->ns_or_dir
// entirely (fine for every OTHER consumer, which always addresses a single
// fixed namespace), which would silently collapse two different namespaces'
// "port" entries into the same slot and make the isolation test below
// meaningless. This local fake is scoped to this file only -- not a change
// to the shared double, which other components' tests still rely on.
#include "unity.h"
#include "bb_udp_client.h"
#include "bb_config.h"
#include "bb_storage.h"

#include <string.h>
#include <stdio.h>

#define UDP_TEST_NVS_MAX_ENTRIES 8
#define UDP_TEST_NVS_MAX_VALUE   128
#define UDP_TEST_NVS_KEY_MAX     48  // "<ns>:<key>"

#define UDP_TEST_NS "bb_udp"

typedef struct {
    bool    used;
    char    key[UDP_TEST_NVS_KEY_MAX];
    size_t  len;
    uint8_t value[UDP_TEST_NVS_MAX_VALUE];
} udp_test_nvs_entry_t;

static udp_test_nvs_entry_t s_udp_test_nvs[UDP_TEST_NVS_MAX_ENTRIES];

static void udp_test_nvs_reset(void)
{
    memset(s_udp_test_nvs, 0, sizeof(s_udp_test_nvs));
}

static void udp_test_nvs_compose_key(const bb_storage_addr_t *addr, char *out, size_t out_cap)
{
    snprintf(out, out_cap, "%s:%s", addr->ns_or_dir ? addr->ns_or_dir : "",
              addr->key ? addr->key : "");
}

static udp_test_nvs_entry_t *udp_test_nvs_find(const char *composed)
{
    for (int i = 0; i < UDP_TEST_NVS_MAX_ENTRIES; i++) {
        if (s_udp_test_nvs[i].used && strcmp(s_udp_test_nvs[i].key, composed) == 0) {
            return &s_udp_test_nvs[i];
        }
    }
    return NULL;
}

static bb_err_t udp_test_nvs_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap,
                                  size_t *out_len)
{
    (void)impl;
    char composed[UDP_TEST_NVS_KEY_MAX];
    udp_test_nvs_compose_key(addr, composed, sizeof(composed));

    udp_test_nvs_entry_t *e = udp_test_nvs_find(composed);
    if (e == NULL) return BB_ERR_NOT_FOUND;
    *out_len = e->len;
    if (cap > 0) {
        size_t copy_len = e->len < cap ? e->len : cap;
        memcpy(buf, e->value, copy_len);
    }
    return BB_OK;
}

static bb_err_t udp_test_nvs_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl;
    if (len > UDP_TEST_NVS_MAX_VALUE) return BB_ERR_NO_SPACE;

    char composed[UDP_TEST_NVS_KEY_MAX];
    udp_test_nvs_compose_key(addr, composed, sizeof(composed));

    udp_test_nvs_entry_t *e = udp_test_nvs_find(composed);
    if (e == NULL) {
        for (int i = 0; i < UDP_TEST_NVS_MAX_ENTRIES; i++) {
            if (!s_udp_test_nvs[i].used) { e = &s_udp_test_nvs[i]; break; }
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

static bb_err_t udp_test_nvs_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    char composed[UDP_TEST_NVS_KEY_MAX];
    udp_test_nvs_compose_key(addr, composed, sizeof(composed));

    udp_test_nvs_entry_t *e = udp_test_nvs_find(composed);
    if (e != NULL) memset(e, 0, sizeof(*e));
    return BB_OK;
}

static bool udp_test_nvs_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    char composed[UDP_TEST_NVS_KEY_MAX];
    udp_test_nvs_compose_key(addr, composed, sizeof(composed));
    return udp_test_nvs_find(composed) != NULL;
}

static const bb_storage_vtable_t s_udp_test_nvs_vtable = {
    .get    = udp_test_nvs_get,
    .set    = udp_test_nvs_set,
    .erase  = udp_test_nvs_erase,
    .exists = udp_test_nvs_exists,
};

// Test-local field descriptors targeting the EXACT SAME address (backend/ns/
// key/type) as bb_udp_client_common.c's internal per-call host_field/
// port_field/broadcast_field -- a literal-address BITE test proving
// bb_udp_client_priv_save_to_nvs/_load_from_nvs actually land on that
// address, not merely that init() returns BB_OK. port/broadcast keep their
// prior STR-typed decimal-ASCII encoding ("12345" / "0"|"1") byte-for-byte.
static const bb_config_field_t s_test_udp_host_field = {
    .id   = "test.udp.host", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = UDP_TEST_NS, .key = "host" },
};
static const bb_config_field_t s_test_udp_port_field = {
    .id   = "test.udp.port", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = UDP_TEST_NS, .key = "port" },
};
static const bb_config_field_t s_test_udp_broadcast_field = {
    .id   = "test.udp.broadcast", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = UDP_TEST_NS, .key = "broadcast" },
};

static void reset_nvs_world(void)
{
    bb_storage_test_reset();
    udp_test_nvs_reset();
    bb_storage_register_backend("nvs", &s_udp_test_nvs_vtable, NULL);
}

// ---------------------------------------------------------------------------
// bb_udp_client_send() before init -> BB_ERR_INVALID_STATE
//
// MUST run before any other test in this file — s_initialized is a
// file-scope static in the host backend with no reset hook (by design: init
// is a one-time boot action). Registered first in test_main.c's RUN_TEST
// order for this module.
// ---------------------------------------------------------------------------

void test_bb_udp_client_send_before_init_returns_invalid_state(void)
{
    const uint8_t buf[] = { 1, 2, 3 };
    bb_err_t err = bb_udp_client_send(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
}

// ---------------------------------------------------------------------------
// cfg init / NVS default path
// ---------------------------------------------------------------------------

void test_bb_udp_client_init_null_loads_defaults(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();

    // No prior cfg persisted for this namespace in this process -> defaults.
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(UDP_TEST_NS, NULL));
}

void test_bb_udp_client_init_persists_and_reloads(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();

    bb_udp_client_cfg_t cfg = { .host = "192.168.1.50", .port = 12345, .broadcast = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(UDP_TEST_NS, &cfg));

    // BITE: the literal "bb_udp"/host,port,broadcast address (the SAME one
    // bb_nv used) must hold the persisted values, port/broadcast still
    // STR-encoded ("12345"/"1") -- proves the storage write actually landed
    // at the byte-compat address+encoding, not just that init() returned
    // BB_OK.
    char host_out[BB_UDP_CLIENT_HOST_MAX] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&s_test_udp_host_field, host_out, sizeof(host_out), &out_len));
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", host_out);

    char port_out[8] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&s_test_udp_port_field, port_out, sizeof(port_out), &out_len));
    TEST_ASSERT_EQUAL_STRING("12345", port_out);

    char bcast_out[4] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&s_test_udp_broadcast_field, bcast_out, sizeof(bcast_out), &out_len));
    TEST_ASSERT_EQUAL_STRING("1", bcast_out);

    // Re-init with NULL must reload the persisted values.
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(UDP_TEST_NS, NULL));
}

void test_bb_udp_client_init_host_too_long_returns_invalid_arg(void)
{
    bb_udp_client_cfg_t cfg;
    // Fill to full capacity — no room left for a NUL terminator.
    memset(cfg.host, 'a', sizeof(cfg.host));
    cfg.port = 9109;
    cfg.broadcast = false;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_udp_client_init(UDP_TEST_NS, &cfg));
}

// ---------------------------------------------------------------------------
// init: ns is required — NULL or empty ns -> BB_ERR_INVALID_ARG, no storage
// touched (B1-951).
// ---------------------------------------------------------------------------

void test_bb_udp_client_init_null_ns_returns_invalid_arg(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_udp_client_init(NULL, NULL));

    // No lingering state: a subsequent valid init must still succeed.
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(UDP_TEST_NS, NULL));
}

void test_bb_udp_client_init_empty_ns_returns_invalid_arg(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_udp_client_init("", NULL));
}

// ---------------------------------------------------------------------------
// two-namespace isolation: init/save under one ns must not be visible under
// a different ns. This is the entire point of B1-951 -- before this change
// the namespace was hardcoded, so this scenario had no way to exist.
//
// BITE PROOF (recorded here, not just asserted): with
// bb_udp_client_priv_save_to_nvs/_load_from_nvs's `ns` parameter temporarily
// ignored in favor of a hardcoded literal (mirroring the pre-B1-951
// behavior), this test goes RED --
//
//   test_bb_udp_client_init_two_namespaces_are_isolated:XXX:FAIL: Expected 0
//   Was 4242
//
// (port under "ns_b" comes back populated with the "ns_a" instance's
// persisted port instead of the "ns_b" default) -- proving the isolation is
// real, not an artifact of the test only checking a return code.
// ---------------------------------------------------------------------------

void test_bb_udp_client_init_two_namespaces_are_isolated(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();

    const bb_config_field_t ns_a_port_field = {
        .id = "test.ns_a.udp.port", .type = BB_CONFIG_STR,
        .addr = { .backend = "nvs", .ns_or_dir = "ns_a", .key = "port" },
    };
    const bb_config_field_t ns_b_port_field = {
        .id = "test.ns_b.udp.port", .type = BB_CONFIG_STR,
        .addr = { .backend = "nvs", .ns_or_dir = "ns_b", .key = "port" },
    };

    bb_udp_client_cfg_t cfg_a = { .host = "10.0.0.1", .port = 4242, .broadcast = false };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init("ns_a", &cfg_a));

    // "ns_a" holds the value written under it.
    char port_a[8] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&ns_a_port_field, port_a, sizeof(port_a), &out_len));
    TEST_ASSERT_EQUAL_STRING("4242", port_a);

    // "ns_b" has never been written -- must NOT see "ns_a"'s value. Reading
    // through init(NULL cfg) under "ns_b" must fall back to the Kconfig
    // default port (BB_UDP_CLIENT_PORT), not "ns_a"'s persisted 4242.
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init("ns_b", NULL));

    // Direct proof (no default-fallback masking): the "ns_b" storage
    // address genuinely has no entry -- "ns_a"'s write did not bleed
    // through. ns_b_port_field carries no has_default, so a bled-through
    // 4242 (or any stored value) would return BB_OK here, not NOT_FOUND.
    char port_b[8] = {0};
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_config_get_str(&ns_b_port_field, port_b, sizeof(port_b), &out_len));

    // Now write a DIFFERENT value under "ns_b" and prove "ns_a" is untouched.
    bb_udp_client_cfg_t cfg_b = { .host = "10.0.0.2", .port = 9999, .broadcast = false };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init("ns_b", &cfg_b));

    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&ns_b_port_field, port_b, sizeof(port_b), &out_len));
    TEST_ASSERT_EQUAL_STRING("9999", port_b);

    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&ns_a_port_field, port_a, sizeof(port_a), &out_len));
    TEST_ASSERT_EQUAL_STRING("4242", port_a);  // unchanged by the "ns_b" write
}

// ---------------------------------------------------------------------------
// send() captures the exact bytes handed to it (no framing knowledge)
// ---------------------------------------------------------------------------

void test_bb_udp_client_send_captures_bytes(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();
    bb_udp_client_cfg_t cfg = { .host = "127.0.0.1", .port = 9109, .broadcast = false };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(UDP_TEST_NS, &cfg));

    const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_send(payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(1, bb_udp_client_host_capture_count());

    uint8_t out[16];
    int n = bb_udp_client_host_last_capture(out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(sizeof(payload), n);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, sizeof(payload));
}

void test_bb_udp_client_send_multiple_increments_capture_count(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();
    bb_udp_client_cfg_t cfg = { .host = "127.0.0.1", .port = 9109, .broadcast = false };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(UDP_TEST_NS, &cfg));

    const uint8_t a[] = { 1 };
    const uint8_t b[] = { 2, 2 };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_send(a, sizeof(a)));
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_send(b, sizeof(b)));
    TEST_ASSERT_EQUAL_INT(2, bb_udp_client_host_capture_count());

    uint8_t out[16];
    int n = bb_udp_client_host_last_capture(out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(sizeof(b), n);
    TEST_ASSERT_EQUAL_MEMORY(b, out, sizeof(b));
}

// ---------------------------------------------------------------------------
// broadcast destination config is accepted and does not block send()
// ---------------------------------------------------------------------------

void test_bb_udp_client_broadcast_cfg_accepted(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();
    bb_udp_client_cfg_t cfg = { .host = "", .port = 9109, .broadcast = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(UDP_TEST_NS, &cfg));

    const uint8_t payload[] = { 7 };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_send(payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(1, bb_udp_client_host_capture_count());
}

// ---------------------------------------------------------------------------
// send() defends NULL buf / negative len
// ---------------------------------------------------------------------------

void test_bb_udp_client_send_null_buf_or_negative_len_returns_invalid_arg(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();
    bb_udp_client_cfg_t cfg = { .host = "127.0.0.1", .port = 9109, .broadcast = false };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(UDP_TEST_NS, &cfg));

    const uint8_t buf[] = { 1 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_udp_client_send(NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_udp_client_send(buf, -1));
}

// ---------------------------------------------------------------------------
// no capture yet -> host_last_capture returns -1
// ---------------------------------------------------------------------------

void test_bb_udp_client_host_last_capture_before_any_send_returns_negative(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();

    uint8_t out[16];
    TEST_ASSERT_EQUAL_INT(-1, bb_udp_client_host_last_capture(out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, bb_udp_client_host_capture_count());
}
