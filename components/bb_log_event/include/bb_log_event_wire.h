#pragma once

// bb_log_event_wire — PUBLIC bb_serialize_desc_t (SSOT) for the "log" bb_data
// key (B1-1045 PR-2, cutover composition-root ownership decision KB 1454).
// The composition root (examples/floor/main/floor_app.c, PR-4) owns wiring
// this descriptor to bb_data via bb_data_bind(). Filed under
// components/bb_log_event/include/ (unlike
// bb_log_event.h's own platform/espidf/-only flat layout) so the portable
// descriptor + host-testable render path are compiled for EVERY board,
// including the host/native test env -- bbtool's board-graph derivation
// globs a component's own include/+src/ unconditionally, but a
// platform/<platform>/<name>/ dir only for boards on that platform layer
// (scripts/bbtool/boards.py derive_component()).
//
// Single-field passthrough: bb_log_event.c's forwarder task already renders
// the full `{"ts","level","tag","msg"}` payload as JSON text before stashing
// it and bumping the "log" bb_data generation (see s_forwarder_task); this
// descriptor re-wraps that already-rendered text as ONE wire string field
// rather than re-decomposing it back into ts/level/tag/msg -- the JSON text
// is opaque to the walker, never re-parsed.

#include "bb_serialize.h"

#include "bb_core.h"

// >= LOG_STREAM_LINE_MAX (192, bb_log_event.c) + JSON object overhead for the
// ts/level/tag/msg wrapper.
#define BB_LOG_EVENT_LOG_TEXT_MAX 220

typedef struct {
    char log[BB_LOG_EVENT_LOG_TEXT_MAX];
} bb_log_event_wire_t;

extern const bb_serialize_desc_t bb_log_event_wire_desc;

#ifdef ESP_PLATFORM
// ESP-IDF only, not host-reproducible: copies the most recently forwarded
// "log" payload (see the s_last_log_json stash in bb_log_event.c, written
// immediately before s_forwarder_task's bb_data_touch("log") call) into
// dst->log. Returns BB_ERR_INVALID_ARG if dst is NULL; otherwise BB_OK, even
// before any log line has ever been forwarded (dst->log reads as an empty
// string in that case).
bb_err_t bb_log_event_gather(bb_log_event_wire_t *dst);
#endif
