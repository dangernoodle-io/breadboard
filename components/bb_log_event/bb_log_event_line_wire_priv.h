#pragma once

// bb_log_event_line_wire — private wire descriptor (SSOT) for the per-log-
// line JSON object s_forwarder_task (platform/espidf/bb_log_event/
// bb_log_event.c) stashes into s_last_log_json before bumping the "log"
// bb_data generation. Migration of that cJSON-built `{"ts","level","tag",
// "msg"}` object to a bb_serialize descriptor rendered into a stack buffer
// -- no heap alloc/free on the log hot path. Field order (ts, level, tag,
// msg) is byte-identical to the prior cJSON output. Portable: no ESP-IDF/
// FreeRTOS types, compiles on host + ESP-IDF.
//
// Distinct from bb_log_event_wire_desc (components/bb_log_event/include/
// bb_log_event_wire.h): that descriptor is the PUBLIC single-field
// passthrough wrapping this line's ALREADY-RENDERED JSON text as one opaque
// wire string for the composition root's bb_data binding -- it never
// re-decomposes back into ts/level/tag/msg. This descriptor is the one that
// actually produces that JSON text in the first place.
//
// Included by:
//   - platform/espidf/bb_log_event/bb_log_event.c (s_forwarder_task)
//   - test/test_host/test_log_event_line_wire.c (expected-JSON fixtures)

#include "bb_serialize.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One forwarded log line, ready to render as
// `{"ts":<int>,"level":"<x>","tag":"...","msg":"..."}`. `level` is a single
// character (I/W/E/D/V or the `?` fallback) plus NUL -- max_len == 2 below.
// `tag`/`msg` sizes mirror s_forwarder_task's own stack locals
// (bb_log_event.c: `char tag[48]`, `char msgbuf[168]`).
typedef struct {
    int64_t ts;
    char    level[2];
    char    tag[48];
    char    msg[168];
} bb_log_event_line_wire_t;

// 4-field row descriptor -- shared by the production forwarder and the host
// tests. Field ORDER (ts, level, tag, msg) matches the prior cJSON build
// exactly.
extern const bb_serialize_field_t bb_log_event_line_wire_fields[4];
// SSOT field count, computed from the array above -- never a hand-typed
// literal, so it can never desync from the array (mirrors
// bb_wifi_http_scan_wire_n_fields's pattern).
extern const uint16_t             bb_log_event_line_wire_n_fields;

// Top-level object descriptor. Renders
// `{"ts":...,"level":"...","tag":"...","msg":"..."}` via
// bb_serialize_json_render().
extern const bb_serialize_desc_t bb_log_event_line_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-2b-i-2) -- co-located JSON
// Schema docs/validation table for bb_log_event_line_wire_desc above, same
// #if-gated pattern as bb_wifi_http_wire_priv.h's exemplar (B1-1059 PR-2a).
// BB_SERIALIZE_META_HOST is a host-only define (set by the PlatformIO native
// env; see platformio.ini) -- NEVER set by the ESP-IDF/device build, so this
// declaration (and its definition in bb_log_event_line_wire.c) compiles to
// nothing on-device.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_log_event_line_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// Worst-case rendered JSON byte size (incl. NUL) for
// bb_log_event_line_wire_desc -- the render-scratch size s_forwarder_task
// (platform/espidf/bb_log_event/bb_log_event.c) and the host tests
// (test_log_event_line_wire.c) both use so bb_serialize_json_render() can
// NEVER return BB_ERR_NO_SPACE for any valid {ts,level,tag,msg} input.
// Derived from bb_serialize_json_bound() (components/bb_serialize_json/src/
// bb_serialize_json.c ~line 710): each string field costs
// `6*strlen(key)+3` for the (worst-case JSON-escaped) key plus
// `6*max_len+2` for the value (6x covers the widest per-byte escape,
// `\uXXXX`), each int field costs 20 (INT64_MIN / UINT64_MAX width), plus
// one trailing comma-or-close byte per field, plus 2 structural braces + 1
// NUL for the object as a whole:
//   ts:    (6*2+3)  + 20            + 1 = 36    // key "ts", int64
//   level: (6*5+3)  + (6*2+2)       + 1 = 48    // key "level", max_len=2
//   tag:   (6*3+3)  + (6*48+2)      + 1 = 312   // key "tag", max_len=48
//   msg:   (6*3+3)  + (6*168+2)     + 1 = 1032  // key "msg", max_len=168
//   fields total                        = 1428
//   + 2 (braces) + 1 (NUL)               = 1431
// bb_log_event.c asserts this constant against a live
// bb_serialize_json_bound(&bb_log_event_line_wire_desc) call at init time
// as a belt-and-suspenders guard against drift.
#define BB_LOG_EVENT_LINE_JSON_MAX 1431

#ifdef __cplusplus
}
#endif
