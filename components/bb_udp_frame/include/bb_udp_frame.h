#pragma once

// bb_udp_frame — pure wire encode/decode for breadboard's UDP telemetry
// transport (ouroboros KB#554). A future encode-side producer and an
// ingress-side decode consumer share the same codec, no mirrored logic.
//
// Wire layout (one self-contained datagram, little-endian):
//
//   magic u16 (0xBBD5) | version u8 (0x01) | kind u8 | flags u8 | seq u32 |
//   topic_len u16 | topic[topic_len] | payload[...to end]
//
// 11-byte fixed header, then topic bytes, then payload runs to end of
// datagram — there is no payload-length field; the datagram boundary (as
// delivered by the transport, e.g. recvfrom()) delimits the payload.
//
// Pure C, no ESP-IDF/platform includes, no heap allocation. Host-testable.

#include "bb_core.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_UDP_FRAME_MAGIC   0xBBD5
#define BB_UDP_FRAME_VERSION 0x01

// Fixed header size in bytes: magic(2) + version(1) + kind(1) + flags(1) +
// seq(4) + topic_len(2).
#define BB_UDP_FRAME_HEADER_LEN 11

typedef enum {
    BB_UDP_KIND_TELEMETRY = 1,
    BB_UDP_KIND_LOG       = 2,
} bb_udp_kind_t;

// flags bit0 — payload is '\n'-delimited log lines (only meaningful when
// kind == BB_UDP_KIND_LOG). Remaining bits reserved; decode does not
// validate reserved bits.
#define BB_UDP_FLAG_LOG_BATCH (1u << 0)

typedef struct {
    bb_udp_kind_t kind;
    uint8_t       flags;

    uint32_t seq;

    // decode: topic/payload point into the caller's dgram buffer (no copy,
    // no ownership transfer — valid only as long as dgram is valid).
    // encode: topic/payload are caller-owned; bb_udp_frame_encode copies
    // their bytes into buf.
    const char *topic;
    uint16_t    topic_len;

    const char *payload;
    uint16_t    payload_len;
} bb_udp_frame_t;

/**
 * Encode one UDP frame into buf.
 *
 * @param in      Frame to encode. topic/payload may be NULL only when their
 *                respective _len is 0.
 * @param buf     Destination buffer.
 * @param buf_cap Capacity of buf, in bytes.
 *
 * @return Number of bytes written (BB_UDP_FRAME_HEADER_LEN + topic_len +
 *         payload_len) on success, or -1 if the encoded frame does not fit
 *         in buf_cap. No partial write occurs on failure. No allocation.
 */
int bb_udp_frame_encode(const bb_udp_frame_t *in, uint8_t *buf, int buf_cap);

/**
 * Decode one UDP datagram into out.
 *
 * Validates: magic == BB_UDP_FRAME_MAGIC, version == BB_UDP_FRAME_VERSION,
 * kind is a member of bb_udp_kind_t, dgram_len >= BB_UDP_FRAME_HEADER_LEN,
 * and the declared topic_len fits within dgram_len (i.e. does not overrun
 * the datagram). On success, out->topic and out->payload point into dgram
 * (no copy) — they are valid only as long as dgram remains valid.
 *
 * @return BB_OK on success, BB_ERR_VALIDATION if any check above fails.
 */
bb_err_t bb_udp_frame_decode(const uint8_t *dgram, int dgram_len, bb_udp_frame_t *out);

#ifdef __cplusplus
}
#endif
