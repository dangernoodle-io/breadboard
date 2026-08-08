// bb_log_emit_build.c — B1-1443 PR-1 pure record-construction helpers for
// bb_log_emit() (platform/espidf/bb_log/bb_log.c). Compiled on both host and
// ESP-IDF (no platform headers, no I/O), following this repo's testability
// convention (rules/embedded.md: "Factor decode/classify/compute logic into
// pure functions... compiled for host + device"). bb_log_emit() itself stays
// a thin ESP-IDF-only wrapper: it stamps ts, formats the message once, then
// calls these to build the console line / event message it fans out to the
// writer queue, bb_diag tap, UDP mirror, and bb_log_event forwarder.

#include "bb_log_internal.h"
#include "bb_str.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

size_t bb_log_emit_build_line(char *out, size_t out_cap, char level_ch, uint64_t ts_ms,
                               const char *tag, const char *msg, size_t msg_len)
{
    if (!out || out_cap == 0) return 0;
    if (!tag) tag = "?";
    // Defensive clamp against a caller-supplied msg_len exceeding the
    // real message length (see the invariant note on the declaration in
    // bb_log_internal.h) -- a no-op for today's only call site, which
    // always derives msg_len from vsnprintf's own return value.
    msg_len = msg ? strnlen(msg, msg_len) : 0;

    int n = snprintf(out, out_cap, "%c (%" PRIu32 ") %s: %.*s\n",
                      level_ch, (uint32_t)ts_ms, tag, (int)msg_len, msg ? msg : "");
    // The format string always emits at least "? (0) ?: \n"-shaped literal
    // decoration regardless of level_ch/tag/msg -- snprintf can never
    // return <= 0 for this fixed shape (unlike bb_log_stream_format(),
    // which formats a caller-supplied fmt string that legitimately can
    // fail), so no defensive n<=0 branch is needed here.
    return ((size_t)n < out_cap) ? (size_t)n : out_cap - 1;
}

void bb_log_emit_build_event_msg(bb_log_event_msg_t *emsg, char level_ch, const char *tag,
                                  uint64_t ts_ms, const char *msg, size_t msg_len)
{
    if (!emsg) return;
    if (!tag) tag = "?";
    // Same defensive clamp as bb_log_emit_build_line() -- see the invariant
    // note on that function's declaration in bb_log_internal.h; identical
    // risk here since emsg->line is filled via memcpy(msg, msg_len).
    msg_len = msg ? strnlen(msg, msg_len) : 0;

    emsg->structured = true;
    emsg->level      = level_ch;
    emsg->ts_ms      = ts_ms;
    bb_strlcpy(emsg->tag, tag, sizeof(emsg->tag));
    emsg->len = (msg_len < sizeof(emsg->line)) ? msg_len : sizeof(emsg->line) - 1;
    if (emsg->len > 0) {
        memcpy(emsg->line, msg, emsg->len);
    }
    emsg->line[emsg->len] = '\0';
}
