#pragma once

// bb_diag_drops_wire — private wire descriptor (SSOT) for the GET
// /api/diag/drops response (B1-1444 PR3): a single aggregation point over
// every drop/failure counter in the log + egress path, sourced from
// bb_log_drop_stats_get() (bb_log.h) and bb_data_http_render_fail_count()/
// bb_data_http_stale_discard_count() (bb_data_http.h). Same
// object-wrap-with-scalar-fields shape as bb_diag_heap_check_wire_priv.h,
// just six fields instead of one.
//
// All six source counters wrap (uint32_t/size_t, never saturate) -- see each
// source's own doc comment (bb_log_drop_stats_t, bb_data_http_render_fail_
// count(), bb_data_http_stale_discard_count()). This route's summary states
// that plainly so a caller doesn't mistake a post-wrap drop in a reading for
// a healthier board.
//
// No "event_pushed" denominator field: bb_data_http_common.c's
// s_event_total_pushed has no public getter (B1-1444 PR3 scope note) -- adding
// one is a separate decision, not made here.
//
// bb_diag_drops_wire_fill() is a pure populate helper -- no ESP-IDF symbols,
// takes the already-gathered raw counters -- so this stays host-testable
// without a live bb_log/bb_data_http instance. Portable: no ESP-IDF/FreeRTOS
// types, compiles on host + ESP-IDF.
//
// Included by:
//   - platform/espidf/bb_diag_http/bb_diag_http_routes.c (the live handler)
//   - test/test_host/test_bb_diag_drops_wire.c (expected-JSON fixtures)
//   - test/test_host/test_bb_diag_drops_wire_meta_golden.c (meta golden)

#include "bb_serialize.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wire snapshot: six cumulative wrapping counters, no backing storage beyond
// itself. All int64_t (BB_TYPE_I64) even though every source counter is
// unsigned -- same widening convention bb_diag_sockets_get_wire_priv.h's
// by_state counts use.
typedef struct {
    int64_t writer_dropped;   // bb_log_drop_stats_t.writer_dropped
    int64_t udp_dropped;      // bb_log_drop_stats_t.udp_dropped
    int64_t event_dropped;    // bb_log_drop_stats_t.event_dropped
    int64_t flush_timeouts;   // bb_log_drop_stats_t.flush_timeouts
    int64_t render_fail;      // bb_data_http_render_fail_count()
    int64_t stale_discard;    // bb_data_http_stale_discard_count()
} bb_diag_drops_wire_t;

// Top-level object descriptor: six BB_TYPE_I64 fields. Renders
// {"writer_dropped":N,"udp_dropped":N,"event_dropped":N,"flush_timeouts":N,
// "render_fail":N,"stale_discard":N} via bb_http_serialize_stream()/
// bb_serialize_json_render().
extern const bb_serialize_desc_t bb_diag_drops_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-3a meta-derivation feeder)
// — co-located JSON Schema docs/validation table for bb_diag_drops_wire_desc
// above, same #if-gated pattern as bb_diag_heap_check_wire_priv.h's
// exemplar. BB_SERIALIZE_META_HOST is a host-only define (set by the
// PlatformIO native env; see platformio.ini) -- NEVER set by the ESP-IDF/
// device build, so this declaration (and its definition in
// bb_diag_drops_wire.c) compiles to nothing on-device.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_diag_drops_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// GET /api/diag/drops RESPONSE schema (B1-1444 PR3, mirrors B1-1059 emit
// batch C site C2's pattern). A #define (not just the accessor below) so
// BOTH this component's config-OFF bb_diag_drops_wire_get_schema() (wire.c)
// AND platform/espidf/bb_diag_http/bb_diag_http_routes.c's static const
// s_drops_get_responses[] table (cross-TU, ESP-IDF-only -- can't call a
// function from a static initializer) can use the SAME literal text as a
// genuine compile-time constant expression.
// ---------------------------------------------------------------------------
#define BB_DIAG_DROPS_SCHEMA_LITERAL \
    "{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"writer_dropped\":{\"type\":\"integer\"}," \
    "\"udp_dropped\":{\"type\":\"integer\"}," \
    "\"event_dropped\":{\"type\":\"integer\"}," \
    "\"flush_timeouts\":{\"type\":\"integer\"}," \
    "\"render_fail\":{\"type\":\"integer\"}," \
    "\"stale_discard\":{\"type\":\"integer\"}}," \
    "\"required\":[\"writer_dropped\",\"udp_dropped\",\"event_dropped\"," \
    "\"flush_timeouts\",\"render_fail\",\"stale_discard\"]}"

// Composed-schema accessor pair (mirrors bb_diag_heap_check_wire_priv.h's
// site-C2 exemplar) -- cross-TU bridge: the register site
// (platform/espidf/bb_diag_http/bb_diag_http_routes.c) is ESP-IDF-only and
// cannot host a compose buffer or call the meta engine directly on host, so
// this portable TU owns the buffer/composer and exposes two accessors.
// bb_diag_drops_wire_get_schema() is ALWAYS declared (zero config-OFF
// footprint beyond the accessor itself: it returns the pre-existing
// BB_DIAG_DROPS_SCHEMA_LITERAL, unchanged); bb_diag_drops_wire_ensure_schema_
// patched() exists ONLY under CONFIG_BB_OPENAPI_RUNTIME_META.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_diag_drops_wire_ensure_schema_patched(void);
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_diag_drops_wire_get_schema(void);

// Pure populate helper: copies each already-gathered source counter into the
// matching wire field. Host-testable without a live bb_log/bb_data_http
// instance -- the sole reason this is factored out of the route handler.
void bb_diag_drops_wire_fill(bb_diag_drops_wire_t *dst,
                              uint32_t writer_dropped, uint32_t udp_dropped,
                              uint32_t event_dropped, uint32_t flush_timeouts,
                              size_t render_fail, uint32_t stale_discard);

#ifdef __cplusplus
}
#endif
