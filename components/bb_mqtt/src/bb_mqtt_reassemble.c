// Pure fragmented-payload reassembly state machine (B1-487 HIGH-1). No
// esp-mqtt / platform types — compiled on host and ESP-IDF so the espidf
// event-handler glue and host unit tests exercise the exact same logic.
#include "bb_mqtt_reassemble.h"
#include "bb_log.h"

#include <string.h>

static const char *TAG = "bb_mqtt_reasm";

void bb_mqtt_reasm_reset(bb_mqtt_reasm_state_t *st)
{
    if (!st) return;
    st->topic[0] = '\0';
    st->total    = 0;
    st->len      = 0;
    st->active   = false;
}

bool bb_mqtt_reasm_step(bb_mqtt_reasm_state_t *st,
                         const char *topic, size_t topic_len,
                         size_t total_len, size_t offset,
                         const void *data, size_t data_len)
{
    if (!st || !st->buf || st->buf_cap == 0) return false;

    if (offset == 0) {
        // First fragment of a new message.
        st->active = true;
        st->len    = 0;
        st->total  = total_len;

        size_t tlen = topic_len;
        if (tlen >= sizeof(st->topic)) tlen = sizeof(st->topic) - 1;
        if (topic && tlen > 0) {
            memcpy(st->topic, topic, tlen);
        }
        st->topic[tlen] = '\0';

        if (st->total > st->buf_cap) {
            bb_log_w(TAG, "rx message too large (topic=%s, total=%u, cap=%u); dropped",
                     st->topic, (unsigned)st->total, (unsigned)st->buf_cap);
            st->active = false;
            return false;
        }
    }

    if (!st->active) return false;  // trailing fragment of an already-dropped message

    // Validate against BOTH the buffer cap and the declared total — a
    // fragment that would push accumulated bytes past the message's own
    // total_len is a protocol violation, not just a buffer-cap concern.
    if (st->len + data_len > st->buf_cap || st->len + data_len > st->total) {
        bb_log_w(TAG, "rx overflow mid-message (topic=%s); dropped", st->topic);
        st->active = false;
        return false;
    }

    if (data && data_len > 0) {
        memcpy(st->buf + st->len, data, data_len);
    }
    st->len += data_len;

    if (st->len >= st->total) {
        st->active = false;
        return true;
    }
    return false;
}
