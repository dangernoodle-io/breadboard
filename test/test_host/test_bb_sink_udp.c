// Tests for bb_sink_udp: UDP telemetry egress sink adapter (host stub —
// captures the encoded datagram instead of opening a real socket).
#include "unity.h"
#include "bb_pub.h"
#include "bb_sink_udp.h"
#include "bb_udp_frame.h"
#include "bb_nv.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Sample functions for tests
// ---------------------------------------------------------------------------

static bool sample_metrics(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "val", 42.0);
    return true;
}

static void setup_sink(bb_pub_sink_t *s)
{
    bb_pub_test_reset();
    bb_sink_udp_test_reset();
    bb_nv_config_set_hostname("udphost");

    bb_sink_udp_cfg_t cfg = { .host = "127.0.0.1", .port = 9109, .broadcast = false };
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_udp_init(&cfg));
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_udp(s));
}

// ---------------------------------------------------------------------------
// publish builds a correct TELEMETRY frame; topic passed through unchanged
// ---------------------------------------------------------------------------

void test_bb_sink_udp_publish_builds_telemetry_frame(void)
{
    bb_pub_sink_t s;
    setup_sink(&s);

    bb_pub_set_sink(&s);
    bb_pub_register_source("cpu", sample_metrics, NULL);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_INT(1, bb_sink_udp_host_capture_count());

    bb_udp_frame_t frame;
    TEST_ASSERT_TRUE(bb_sink_udp_host_last_frame(&frame));
    TEST_ASSERT_EQUAL_INT(BB_UDP_KIND_TELEMETRY, frame.kind);
    TEST_ASSERT_EQUAL_UINT8(0, frame.flags);

    // Topic must be passed through unchanged: "metrics/udphost/cpu"
    char topic[64];
    memcpy(topic, frame.topic, frame.topic_len);
    topic[frame.topic_len] = '\0';
    TEST_ASSERT_EQUAL_STRING("metrics/udphost/cpu", topic);

    // Payload must be intact JSON.
    TEST_ASSERT_TRUE(frame.payload_len > 0);
    char payload[256];
    memcpy(payload, frame.payload, frame.payload_len);
    payload[frame.payload_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"val\""));
}

// ---------------------------------------------------------------------------
// publish() defends NULL topic and negative len (defensive branches not
// reachable via the normal bb_pub tick path, which always supplies a
// non-NULL topic and len >= 0) — call the sink's publish fn directly.
// ---------------------------------------------------------------------------

void test_bb_sink_udp_publish_null_topic_and_negative_len(void)
{
    bb_pub_sink_t s;
    setup_sink(&s);

    bb_err_t rc = s.publish(s.ctx, NULL, "{}", -1, false);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    TEST_ASSERT_EQUAL_INT(1, bb_sink_udp_host_capture_count());
    bb_udp_frame_t frame;
    TEST_ASSERT_TRUE(bb_sink_udp_host_last_frame(&frame));
    TEST_ASSERT_EQUAL_UINT16(0, frame.topic_len);
    TEST_ASSERT_EQUAL_UINT16(0, frame.payload_len);
}

// ---------------------------------------------------------------------------
// seq increments across successive publishes
// ---------------------------------------------------------------------------

void test_bb_sink_udp_seq_increments_across_publishes(void)
{
    bb_pub_sink_t s;
    setup_sink(&s);

    bb_pub_set_sink(&s);
    bb_pub_register_source("cpu", sample_metrics, NULL);

    bb_pub_tick_once();
    bb_udp_frame_t frame1;
    TEST_ASSERT_TRUE(bb_sink_udp_host_last_frame(&frame1));

    bb_pub_tick_once();
    bb_udp_frame_t frame2;
    TEST_ASSERT_TRUE(bb_sink_udp_host_last_frame(&frame2));

    TEST_ASSERT_EQUAL_UINT32(frame1.seq + 1, frame2.seq);
}

// ---------------------------------------------------------------------------
// Oversized topic+payload (> MTU) -> dropped, dropped() increments, nothing
// "sent" (capture count unchanged)
// ---------------------------------------------------------------------------

// 1400 mirrors the default CONFIG_BB_SINK_UDP_MTU (Kconfig); +64 guarantees
// the encoded frame (header + topic + payload) exceeds the MTU regardless of
// the host stub's compiled buffer size.
static char s_big_payload[1400 + 64];

static bool sample_oversized(bb_json_t obj, void *ctx)
{
    (void)ctx;
    memset(s_big_payload, 'x', sizeof(s_big_payload) - 1);
    s_big_payload[sizeof(s_big_payload) - 1] = '\0';
    bb_json_obj_set_string(obj, "blob", s_big_payload);
    return true;
}

void test_bb_sink_udp_oversized_frame_dropped(void)
{
    bb_pub_sink_t s;
    setup_sink(&s);

    TEST_ASSERT_EQUAL_UINT32(0, bb_sink_udp_dropped());

    bb_pub_set_sink(&s);
    bb_pub_register_source("big", sample_oversized, NULL);

    bb_pub_tick_once();

    TEST_ASSERT_EQUAL_UINT32(1, bb_sink_udp_dropped());
    TEST_ASSERT_EQUAL_INT(0, bb_sink_udp_host_capture_count());
}

// ---------------------------------------------------------------------------
// cfg init / NVS default path
// ---------------------------------------------------------------------------

void test_bb_sink_udp_init_null_loads_defaults(void)
{
    bb_pub_test_reset();
    bb_sink_udp_test_reset();

    // No prior cfg persisted for this namespace in this process -> defaults.
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_udp_init(NULL));

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_udp(&s));
    TEST_ASSERT_EQUAL_STRING("udp", s.transport);
    TEST_ASSERT_FALSE(s.tls);
}

void test_bb_sink_udp_init_persists_and_reloads(void)
{
    bb_pub_test_reset();
    bb_sink_udp_test_reset();

    bb_sink_udp_cfg_t cfg = { .host = "192.168.1.50", .port = 12345, .broadcast = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_udp_init(&cfg));

    // Re-init with NULL must reload the persisted values.
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_udp_init(NULL));

    bb_pub_sink_t s;
    TEST_ASSERT_EQUAL(BB_OK, bb_sink_udp(&s));
}

void test_bb_sink_udp_init_host_too_long_returns_invalid_arg(void)
{
    bb_sink_udp_cfg_t cfg;
    // Fill to full capacity — no room left for a NUL terminator.
    memset(cfg.host, 'a', sizeof(cfg.host));
    cfg.port = 9109;
    cfg.broadcast = false;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_sink_udp_init(&cfg));
}

// ---------------------------------------------------------------------------
// bb_sink_udp() before init -> BB_ERR_INVALID_STATE
//
// MUST run before any other test in this file calls bb_sink_udp_init() —
// s_initialized is a file-scope static in the host backend with no reset
// hook (by design: init is a one-time boot action). Registered first in
// test_main.c's RUN_TEST order for this module.
// ---------------------------------------------------------------------------

void test_bb_sink_udp_before_init_returns_invalid_state(void)
{
    bb_pub_sink_t s;
    bb_err_t err = bb_sink_udp(&s);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
}

// ---------------------------------------------------------------------------
// bb_sink_udp(NULL) -> BB_ERR_INVALID_ARG (checked before init-state)
// ---------------------------------------------------------------------------

void test_bb_sink_udp_null_out_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_sink_udp(NULL));
}
