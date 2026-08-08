// bb_log_event_line_wire_build.c — B1-1443 PR-2 pure record-construction
// helper for s_forwarder_task's foreign/vendored-line branch
// (platform/espidf/bb_log_event/bb_log_event.c). Compiled on both host and
// ESP-IDF (no platform headers, no I/O), following this repo's testability
// convention (rules/embedded.md: "Factor decode/classify/compute logic into
// pure functions... compiled for host + device") -- the same pattern
// bb_log_emit_build.c already applies to bb_log_emit()'s structured record
// construction. s_forwarder_task itself stays a thin ESP-IDF-only wrapper:
// it drains the queue, stamps the drain-time ts, and calls this to build the
// wire snap it renders through bb_log_event_line_wire_desc.

#include "../bb_log_event_line_wire_priv.h"

#include <string.h>

void bb_log_event_line_wire_build_foreign(bb_log_event_line_wire_t *snap, int64_t ts,
                                           const char *line, size_t line_len)
{
    if (!snap) return;

    // Every field is set below -- start from a clean slate so a caller's
    // stale snap contents can never leak through.
    memset(snap, 0, sizeof(*snap));

    snap->ts = ts;

    // Opaque wrap: no re-parse is ever attempted for a foreign line, so
    // level is always the '?' fallback letter and tag is always empty
    // (already '\0' from the memset above -- set explicitly so the "no tag"
    // invariant is visible at the call site, not just an artifact of
    // zero-init).
    snap->level[0] = '?';
    snap->level[1] = '\0';
    snap->tag[0]   = '\0';

    // Defensive clamp against a caller-supplied line_len exceeding the real
    // line length (see the invariant note on the declaration in
    // bb_log_event_line_wire_priv.h) -- same rationale as
    // bb_log_emit_build_line()'s/bb_log_emit_build_event_msg()'s identical
    // strnlen guard (components/bb_log/src/bb_log_emit_build.c).
    line_len = line ? strnlen(line, line_len) : 0;

    // Raw line copied verbatim (no ANSI stripping, no CRLF trimming, no
    // field extraction) -- memcpy + explicit NUL, not bb_strlcpy: line is
    // not guaranteed NUL-terminated within line_len bytes, only within its
    // own buffer, so the length this function was actually told to trust is
    // the one that governs truncation, not strlen(line) beyond it.
    size_t copy_len = (line_len < sizeof(snap->msg)) ? line_len : sizeof(snap->msg) - 1;
    if (copy_len > 0) {
        memcpy(snap->msg, line, copy_len);
    }
    snap->msg[copy_len] = '\0';
}
