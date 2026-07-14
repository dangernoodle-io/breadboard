// Tests for bb_udp_client: reusable IPv4 UDP datagram transport (host stub —
// captures the raw bytes handed to bb_udp_client_send() instead of opening a
// real socket). These cases exercise the transport directly: socket
// lifecycle, dest config, capture buffer.
//
// B1-756 (bb_nv dissolution epic B1-708): host/port/broadcast now round-trip
// through bb_config (backend="nvs") instead of bb_nv's generic KV forwarder
// -- fake_nvs_backend.h's fake "nvs" backend is registered below so the NVS
// load/persist tests exercise a real backend instead of a no-op (the real
// "nvs" bb_storage backend is ESP-IDF-only).
#include "unity.h"
#include "bb_udp_client.h"
#include "bb_config.h"
#include "bb_storage.h"
#include "fake_nvs_backend.h"

#include <string.h>

// Test-local field descriptors targeting the EXACT SAME address (backend/ns/
// key/type) as bb_udp_client_common.c's internal s_udp_host_field/
// s_udp_port_field/s_udp_broadcast_field -- a literal-address BITE test
// proving bb_udp_client_priv_save_to_nvs/_load_from_nvs actually land on
// that address. port/broadcast keep their prior STR-typed decimal-ASCII
// encoding ("12345" / "0"|"1") byte-for-byte.
static const bb_config_field_t s_test_udp_host_field = {
    .id   = "test.udp.host", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_udp", .key = "host" },
};
static const bb_config_field_t s_test_udp_port_field = {
    .id   = "test.udp.port", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_udp", .key = "port" },
};
static const bb_config_field_t s_test_udp_broadcast_field = {
    .id   = "test.udp.broadcast", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_udp", .key = "broadcast" },
};

static void reset_nvs_world(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
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
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(NULL));
}

void test_bb_udp_client_init_persists_and_reloads(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();

    bb_udp_client_cfg_t cfg = { .host = "192.168.1.50", .port = 12345, .broadcast = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(&cfg));

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
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(NULL));
}

void test_bb_udp_client_init_host_too_long_returns_invalid_arg(void)
{
    bb_udp_client_cfg_t cfg;
    // Fill to full capacity — no room left for a NUL terminator.
    memset(cfg.host, 'a', sizeof(cfg.host));
    cfg.port = 9109;
    cfg.broadcast = false;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_udp_client_init(&cfg));
}

// ---------------------------------------------------------------------------
// send() captures the exact bytes handed to it (no framing knowledge)
// ---------------------------------------------------------------------------

void test_bb_udp_client_send_captures_bytes(void)
{
    bb_udp_client_test_reset();
    reset_nvs_world();
    bb_udp_client_cfg_t cfg = { .host = "127.0.0.1", .port = 9109, .broadcast = false };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(&cfg));

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
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(&cfg));

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
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(&cfg));

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
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(&cfg));

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
