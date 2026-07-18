// Pure bb_format_t <-> name mapping. No platform/heap/lock dependency --
// compiles identically on host, ESP-IDF, and Arduino.
#include "bb_format.h"

#include <stddef.h>

// Compile-time guard: the table below must carry exactly one entry per real
// bb_format_t value (everything except the BB_FORMAT__COUNT sentinel).
// Catches a future enum addition that forgets to extend the table.
// When adding a new bb_format_t value, extend s_bb_format_names AND bump the
// literal below to match the new BB_FORMAT__COUNT.
_Static_assert(BB_FORMAT__COUNT == 4,
               "bb_format_name's table must be extended to cover every new bb_format_t value");

static const char *const s_bb_format_names[BB_FORMAT__COUNT] = {
    [BB_FORMAT_NONE]    = NULL,   // no wire format -- nothing to name
    [BB_FORMAT_JSON]    = "json",
    [BB_FORMAT_CONSOLE] = "console",
    [BB_FORMAT_LOGFMT]  = "logfmt",
};

const char *bb_format_name(bb_format_t fmt)
{
    if ((unsigned)fmt >= (unsigned)BB_FORMAT__COUNT) return NULL;
    return s_bb_format_names[fmt];
}
