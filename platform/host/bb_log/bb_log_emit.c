// bb_log_emit — host backend (B1-1443 PR-1). Host has no console-writer
// queue, bb_diag panic tap, UDP mirror, or bb_log_event forwarder to fan
// out to -- it only ever had a direct fprintf, so bb_log_emit() here is
// just that, given a stable name so bb_log_e/w/i (bb_log.h's host branch)
// share the same "single emit function" contract ESP-IDF has, instead of
// diverging into their own macro-embedded fprintf calls. No runtime per-tag
// filter on host (bb_log_d/v are already compiled-out no-ops in bb_log.h,
// unchanged by this file) -- do not invent a host tag registry here.

#include "bb_log.h"
#include "../../../components/bb_log/src/bb_log_internal.h"

#include <stdio.h>

void bb_log_emit(bb_log_level_t bb_level, const char *tag, const char *fmt, ...)
{
    if (!tag) tag = "?";

    FILE *out = (bb_level == BB_LOG_LEVEL_INFO) ? stdout : stderr;
    char level_ch = bb_log_level_console_letter(bb_level);

    fprintf(out, "%c (%s) ", level_ch, tag);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fputc('\n', out);
}
