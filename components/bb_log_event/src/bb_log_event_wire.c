// bb_log_event_wire — the format-agnostic "log" topic descriptor SSOT. See
// bb_log_event_wire.h for the wire-struct contract. Compiles on both host
// and ESP-IDF; no platform-specific code (the ESP-IDF-only gather lives in
// bb_log_event.c, guarded by ESP_PLATFORM, next to the stash it reads).

#include "bb_log_event_wire.h"

#include <stddef.h>

static const bb_serialize_field_t s_log_event_wire_fields[] = {
    { .key = "log", .type = BB_TYPE_STR,
      .offset = offsetof(bb_log_event_wire_t, log), .max_len = BB_LOG_EVENT_LOG_TEXT_MAX },
};

const bb_serialize_desc_t bb_log_event_wire_desc = {
    .type_name = "log",
    .fields    = s_log_event_wire_fields,
    .n_fields  = sizeof(s_log_event_wire_fields) / sizeof(s_log_event_wire_fields[0]),
    .snap_size = sizeof(bb_log_event_wire_t),
};
