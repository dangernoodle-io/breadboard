#pragma once

// bb_log_event_wire — PUBLIC bb_serialize_desc_t (SSOT) for the "log" bb_event
// topic (B1-1045 PR-2, cutover composition-root ownership decision KB 1454).
// ADDITIVE-only, INERT: no bb_data_bind() call exists anywhere in this PR --
// the composition root (PR-4) is the sole owner of wiring this descriptor to
// bb_data. Filed under components/bb_log_event/include/ (unlike
// bb_log_event.h's own platform/espidf/-only flat layout) so the portable
// descriptor + host-testable render path are compiled for EVERY board,
// including the host/native test env -- bbtool's board-graph derivation
// globs a component's own include/+src/ unconditionally, but a
// platform/<platform>/<name>/ dir only for boards on that platform layer
// (scripts/bbtool/boards.py derive_component()).
//
// Single-field passthrough: bb_log_event.c's forwarder task already renders
// the full `{"ts","level","tag","msg"}` payload as JSON text before posting
// it on the "log" bb_event topic (see s_forwarder_task); this descriptor
// re-wraps that already-rendered text as ONE wire string field rather than
// re-decomposing it back into ts/level/tag/msg -- the JSON text is opaque to
// the walker, never re-parsed.

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
// "log" payload (see the s_last_log_json stash in bb_log_event.c,
// immediately after s_forwarder_task's existing bb_event_post() call) into
// dst->log. Returns BB_ERR_INVALID_ARG if dst is NULL; otherwise BB_OK, even
// before any log line has ever been forwarded (dst->log reads as an empty
// string in that case).
bb_err_t bb_log_event_gather(bb_log_event_wire_t *dst);
#endif
