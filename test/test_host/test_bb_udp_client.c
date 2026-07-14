// Tests for bb_udp_client: reusable IPv4 UDP datagram transport (host stub —
// captures the raw bytes handed to bb_udp_client_send() instead of opening a
// real socket). These cases exercise the transport directly: socket
// lifecycle, dest config, capture buffer.
#include "unity.h"
#include "bb_udp_client.h"

#include <string.h>

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

    // No prior cfg persisted for this namespace in this process -> defaults.
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(NULL));
}

void test_bb_udp_client_init_persists_and_reloads(void)
{
    bb_udp_client_test_reset();

    bb_udp_client_cfg_t cfg = { .host = "192.168.1.50", .port = 12345, .broadcast = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_udp_client_init(&cfg));

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

    uint8_t out[16];
    TEST_ASSERT_EQUAL_INT(-1, bb_udp_client_host_last_capture(out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, bb_udp_client_host_capture_count());
}
