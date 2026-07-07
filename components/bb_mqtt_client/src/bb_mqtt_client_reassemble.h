// Private header — not part of the public bb_mqtt_client surface. Lives next to the
// implementation (components/bb_mqtt_client/src/), not in include/.
#pragma once

#include <stddef.h>
#include <stdbool.h>

#define BB_MQTT_CLIENT_REASM_TOPIC_MAX 128

// Pure fragmented-payload reassembly state, shared verbatim by the espidf
// event-handler glue (MQTT_EVENT_DATA case) and host unit tests so the
// reassembly state machine has exactly one implementation (B1-487 HIGH-1).
//
// buf/buf_cap point at a caller-owned buffer — this struct itself allocates
// nothing. Callers (one per bb_mqtt_client handle, B1-487 HIGH-2) lazily allocate
// buf and keep it for the handle's lifetime.
typedef struct {
    char   topic[BB_MQTT_CLIENT_REASM_TOPIC_MAX];
    size_t total;    // expected total payload size for the in-flight message
    size_t len;      // bytes accumulated so far
    bool   active;   // true while accumulating; false once dispatched/dropped
    char  *buf;      // caller-owned reassembly buffer (NULL = not allocated yet)
    size_t buf_cap;  // capacity of buf, in bytes
} bb_mqtt_client_reasm_state_t;

// Reset accumulation fields (buf/buf_cap are left untouched — call once right
// after allocating/assigning the buffer, not per-message).
void bb_mqtt_client_reasm_reset(bb_mqtt_client_reasm_state_t *st);

// Process one MQTT_EVENT_DATA-shaped fragment.
//
// topic/topic_len — only meaningful (and only read) when offset==0 (esp-mqtt
//                   supplies the topic only on the first fragment).
// total_len       — event->total_data_len; the full message size.
// offset          — event->current_data_offset.
// data/data_len   — this fragment's bytes.
//
// Returns true when the message is now fully reassembled — st->topic/st->buf
// (st->len bytes) describe the complete message and the caller should invoke
// its callback. Returns false while still accumulating, or when the fragment
// was dropped (declared total exceeds buf_cap, or a mid-message overflow
// against buf_cap/total) — st->active is false in the dropped case so
// trailing fragments of the same dropped message are ignored.
//
// No-op (returns false) if st->buf is NULL or st->buf_cap is 0 — caller must
// allocate the buffer before routing fragments here.
bool bb_mqtt_client_reasm_step(bb_mqtt_client_reasm_state_t *st,
                         const char *topic, size_t topic_len,
                         size_t total_len, size_t offset,
                         const void *data, size_t data_len);
