#include "unity.h"
#include "bb_udp_frame.h"
#include "bb_byte_order.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Round-trip encode -> decode
// ---------------------------------------------------------------------------

void test_bb_udp_frame_round_trip_telemetry(void)
{
    uint8_t buf[128];
    const char *topic   = "wifi.rssi";
    const char *payload = "{\"rssi\":-55}";

    bb_udp_frame_t in = {
        .kind        = BB_UDP_KIND_TELEMETRY,
        .flags       = 0,
        .seq         = 4242,
        .topic       = topic,
        .topic_len   = (uint16_t)strlen(topic),
        .payload     = payload,
        .payload_len = (uint16_t)strlen(payload),
    };

    int n = bb_udp_frame_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(BB_UDP_FRAME_HEADER_LEN + (int)in.topic_len + (int)in.payload_len, n);

    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, n, &out);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(BB_UDP_KIND_TELEMETRY, out.kind);
    TEST_ASSERT_EQUAL_UINT8(0, out.flags);
    TEST_ASSERT_EQUAL_UINT32(4242, out.seq);
    TEST_ASSERT_EQUAL_UINT16(strlen(topic), out.topic_len);
    TEST_ASSERT_EQUAL_MEMORY(topic, out.topic, out.topic_len);
    TEST_ASSERT_EQUAL_UINT16(strlen(payload), out.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, out.payload, out.payload_len);
}

void test_bb_udp_frame_round_trip_log_batch(void)
{
    uint8_t buf[128];
    const char *topic   = "log";
    const char *payload = "line one\nline two\n";

    bb_udp_frame_t in = {
        .kind        = BB_UDP_KIND_LOG,
        .flags       = BB_UDP_FLAG_LOG_BATCH,
        .seq         = 7,
        .topic       = topic,
        .topic_len   = (uint16_t)strlen(topic),
        .payload     = payload,
        .payload_len = (uint16_t)strlen(payload),
    };

    int n = bb_udp_frame_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, n, &out);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(BB_UDP_KIND_LOG, out.kind);
    TEST_ASSERT_EQUAL_UINT8(BB_UDP_FLAG_LOG_BATCH, out.flags);
    TEST_ASSERT_EQUAL_UINT32(7, out.seq);
    TEST_ASSERT_EQUAL_MEMORY(payload, out.payload, out.payload_len);
}

// ---------------------------------------------------------------------------
// Encode: -1 when it won't fit — both arms
// ---------------------------------------------------------------------------

void test_bb_udp_frame_encode_header_wont_fit_returns_neg1(void)
{
    uint8_t buf[BB_UDP_FRAME_HEADER_LEN - 1];
    bb_udp_frame_t in = {
        .kind = BB_UDP_KIND_TELEMETRY, .flags = 0, .seq = 0,
        .topic = NULL, .topic_len = 0, .payload = NULL, .payload_len = 0,
    };
    int n = bb_udp_frame_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_bb_udp_frame_encode_topic_payload_overflow_returns_neg1(void)
{
    uint8_t buf[BB_UDP_FRAME_HEADER_LEN + 3];  // header fits, body does not
    const char *topic   = "abcd";
    const char *payload = "x";
    bb_udp_frame_t in = {
        .kind = BB_UDP_KIND_TELEMETRY, .flags = 0, .seq = 0,
        .topic = topic, .topic_len = 4, .payload = payload, .payload_len = 1,
    };
    int n = bb_udp_frame_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, n);
}

// ---------------------------------------------------------------------------
// Decode rejects
// ---------------------------------------------------------------------------

void test_bb_udp_frame_decode_short_header_returns_validation(void)
{
    uint8_t buf[BB_UDP_FRAME_HEADER_LEN - 1];
    memset(buf, 0, sizeof(buf));
    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION, rc);
}

void test_bb_udp_frame_decode_bad_magic_returns_validation(void)
{
    uint8_t buf[BB_UDP_FRAME_HEADER_LEN];
    memset(buf, 0, sizeof(buf));
    bb_store_le16(buf, 0xDEAD);
    buf[2] = BB_UDP_FRAME_VERSION;
    buf[3] = BB_UDP_KIND_TELEMETRY;
    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION, rc);
}

void test_bb_udp_frame_decode_bad_version_returns_validation(void)
{
    uint8_t buf[BB_UDP_FRAME_HEADER_LEN];
    memset(buf, 0, sizeof(buf));
    bb_store_le16(buf, BB_UDP_FRAME_MAGIC);
    buf[2] = 0x02;  // not BB_UDP_FRAME_VERSION
    buf[3] = BB_UDP_KIND_TELEMETRY;
    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION, rc);
}

void test_bb_udp_frame_decode_bad_kind_returns_validation(void)
{
    uint8_t buf[BB_UDP_FRAME_HEADER_LEN];
    memset(buf, 0, sizeof(buf));
    bb_store_le16(buf, BB_UDP_FRAME_MAGIC);
    buf[2] = BB_UDP_FRAME_VERSION;
    buf[3] = 0x09;  // not a member of bb_udp_kind_t
    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION, rc);
}

void test_bb_udp_frame_decode_topic_len_overruns_dgram_returns_validation(void)
{
    uint8_t buf[BB_UDP_FRAME_HEADER_LEN];  // no room for any topic bytes
    memset(buf, 0, sizeof(buf));
    bb_store_le16(buf, BB_UDP_FRAME_MAGIC);
    buf[2] = BB_UDP_FRAME_VERSION;
    buf[3] = BB_UDP_KIND_TELEMETRY;
    bb_store_le16(buf + 9, 1);  // claims 1 topic byte that doesn't exist
    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(BB_ERR_VALIDATION, rc);
}

// ---------------------------------------------------------------------------
// Boundary cases
// ---------------------------------------------------------------------------

void test_bb_udp_frame_encode_decode_exact_buffer_fit(void)
{
    uint8_t topic_bytes[300];
    uint8_t payload_bytes[500];
    memset(topic_bytes, 'a', sizeof(topic_bytes));
    memset(payload_bytes, 'b', sizeof(payload_bytes));

    uint8_t buf[BB_UDP_FRAME_HEADER_LEN + sizeof(topic_bytes) + sizeof(payload_bytes)];

    bb_udp_frame_t in = {
        .kind        = BB_UDP_KIND_TELEMETRY,
        .flags       = 0,
        .seq         = 1,
        .topic       = (const char *)topic_bytes,
        .topic_len   = sizeof(topic_bytes),
        .payload     = (const char *)payload_bytes,
        .payload_len = sizeof(payload_bytes),
    };

    int n = bb_udp_frame_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)sizeof(buf), n);

    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, n, &out);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(sizeof(topic_bytes), out.topic_len);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload_bytes), out.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(topic_bytes, out.topic, out.topic_len);
    TEST_ASSERT_EQUAL_MEMORY(payload_bytes, out.payload, out.payload_len);
}

void test_bb_udp_frame_encode_decode_empty_payload(void)
{
    uint8_t buf[64];
    const char *topic = "info";
    bb_udp_frame_t in = {
        .kind = BB_UDP_KIND_TELEMETRY, .flags = 0, .seq = 0,
        .topic = topic, .topic_len = (uint16_t)strlen(topic),
        .payload = NULL, .payload_len = 0,
    };

    int n = bb_udp_frame_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, n, &out);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(0, out.payload_len);
}

// Empty topic is a valid, legitimate encoding — topic_len 0 is not
// special-cased or rejected; the whole datagram body is payload.
void test_bb_udp_frame_encode_decode_empty_topic(void)
{
    uint8_t buf[64];
    const char *payload = "hello";
    bb_udp_frame_t in = {
        .kind = BB_UDP_KIND_TELEMETRY, .flags = 0, .seq = 0,
        .topic = NULL, .topic_len = 0,
        .payload = payload, .payload_len = (uint16_t)strlen(payload),
    };

    int n = bb_udp_frame_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    bb_udp_frame_t out;
    bb_err_t rc = bb_udp_frame_decode(buf, n, &out);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(0, out.topic_len);
    TEST_ASSERT_EQUAL_UINT16(strlen(payload), out.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, out.payload, out.payload_len);
}

// ---------------------------------------------------------------------------
// Little-endian byte layout — assert exact bytes in the encoded buffer
// ---------------------------------------------------------------------------

void test_bb_udp_frame_encode_little_endian_layout(void)
{
    uint8_t buf[BB_UDP_FRAME_HEADER_LEN + 2];
    const char *topic = "ab";
    bb_udp_frame_t in = {
        .kind        = BB_UDP_KIND_LOG,
        .flags       = 0x03,
        .seq         = 0x11223344,
        .topic       = topic,
        .topic_len   = 2,
        .payload     = NULL,
        .payload_len = 0,
    };

    int n = bb_udp_frame_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(BB_UDP_FRAME_HEADER_LEN + 2, n);

    // magic (LE u16): 0xBBD5 -> bytes 0xD5, 0xBB
    TEST_ASSERT_EQUAL_UINT8(0xD5, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, buf[1]);
    // version
    TEST_ASSERT_EQUAL_UINT8(BB_UDP_FRAME_VERSION, buf[2]);
    // kind
    TEST_ASSERT_EQUAL_UINT8(BB_UDP_KIND_LOG, buf[3]);
    // flags
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[4]);
    // seq (LE u32): 0x11223344 -> bytes 44 33 22 11
    TEST_ASSERT_EQUAL_UINT8(0x44, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(0x33, buf[6]);
    TEST_ASSERT_EQUAL_UINT8(0x22, buf[7]);
    TEST_ASSERT_EQUAL_UINT8(0x11, buf[8]);
    // topic_len (LE u16): 2 -> bytes 02 00
    TEST_ASSERT_EQUAL_UINT8(0x02, buf[9]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[10]);
    // topic bytes
    TEST_ASSERT_EQUAL_UINT8('a', buf[11]);
    TEST_ASSERT_EQUAL_UINT8('b', buf[12]);
}
