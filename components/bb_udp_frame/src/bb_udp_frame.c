#include "bb_udp_frame.h"
#include "bb_byte_order.h"

#include <string.h>

int bb_udp_frame_encode(const bb_udp_frame_t *in, uint8_t *buf, int buf_cap)
{
    if (buf_cap < BB_UDP_FRAME_HEADER_LEN) return -1;

    int total = BB_UDP_FRAME_HEADER_LEN + (int)in->topic_len + (int)in->payload_len;
    if (total > buf_cap) return -1;

    bb_store_le16(buf, BB_UDP_FRAME_MAGIC);
    buf[2] = BB_UDP_FRAME_VERSION;
    buf[3] = (uint8_t)in->kind;
    buf[4] = in->flags;
    bb_store_le32(buf + 5, in->seq);
    bb_store_le16(buf + 9, in->topic_len);

    uint8_t *p = buf + BB_UDP_FRAME_HEADER_LEN;
    if (in->topic_len > 0) {
        memcpy(p, in->topic, in->topic_len);
        p += in->topic_len;
    }
    if (in->payload_len > 0) {
        memcpy(p, in->payload, in->payload_len);
    }

    return total;
}

bb_err_t bb_udp_frame_decode(const uint8_t *dgram, int dgram_len, bb_udp_frame_t *out)
{
    if (dgram_len < BB_UDP_FRAME_HEADER_LEN) return BB_ERR_VALIDATION;

    uint16_t magic = bb_load_le16(dgram);
    if (magic != BB_UDP_FRAME_MAGIC) return BB_ERR_VALIDATION;

    uint8_t version = dgram[2];
    if (version != BB_UDP_FRAME_VERSION) return BB_ERR_VALIDATION;

    uint8_t kind = dgram[3];
    if (kind != BB_UDP_KIND_TELEMETRY && kind != BB_UDP_KIND_LOG) return BB_ERR_VALIDATION;

    uint8_t  flags     = dgram[4];
    uint32_t seq       = bb_load_le32(dgram + 5);
    uint16_t topic_len = bb_load_le16(dgram + 9);

    if ((int)topic_len > dgram_len - BB_UDP_FRAME_HEADER_LEN) return BB_ERR_VALIDATION;

    out->kind        = (bb_udp_kind_t)kind;
    out->flags       = flags;
    out->seq         = seq;
    out->topic       = (const char *)(dgram + BB_UDP_FRAME_HEADER_LEN);
    out->topic_len   = topic_len;
    out->payload     = (const char *)(dgram + BB_UDP_FRAME_HEADER_LEN + topic_len);
    out->payload_len = (uint16_t)(dgram_len - BB_UDP_FRAME_HEADER_LEN - topic_len);

    return BB_OK;
}
