// bb_log's TELEM wide-routing decision core -- pure, portable (no platform
// dependency), host-testable. See bb_log_internal.h for the full rationale.
// B1-831 PR-2 built the decision function + its Kconfig bridge. B1-831 PR-3
// adds the runtime enable/disable state plus the line-level combinator
// (bb_log_telem_should_route_wide) that the s_log_vprintf call site
// (platform/espidf/bb_log/bb_log.c) actually calls -- the same function is
// exercised directly by host tests, so the call site and the tests share one
// code path rather than a mirrored one.
#include "bb_log_internal.h"

#include <string.h>

// Kconfig -> C bridge. C default matches the Kconfig default (mandatory
// pattern -- never shadow the generated CONFIG_ symbol with a bare #ifndef).
// Same pattern bb_log_config.c already uses for BB_LOG_DEFAULT_LEVEL_STR.
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_LOG_TELEM_TAG
#define BB_LOG_TELEM_TAG_STR CONFIG_BB_LOG_TELEM_TAG
#endif
#endif

#ifndef BB_LOG_TELEM_TAG_STR
#define BB_LOG_TELEM_TAG_STR "TELEM"
#endif

// CONFIG_BB_LOG_TELEM_TAG is a free-form Kconfig string with no length
// bound, but bb_log_telem_should_route_wide() parses the line's tag into a
// fixed BB_LOG_TELEM_TAG_PARSE_CAP-byte buffer (bb_log_internal.h). An
// over-long configured tag would silently truncate on parse and never match
// via strcmp() below, making the TELEM gate permanently, silently inert
// (every line routes wide, no build warning, no runtime log). Fail the build
// loudly instead.
_Static_assert(sizeof(BB_LOG_TELEM_TAG_STR) <= BB_LOG_TELEM_TAG_PARSE_CAP,
                "BB_LOG_TELEM_TAG must fit in BB_LOG_TELEM_TAG_PARSE_CAP bytes (see Kconfig help)");

bool bb_log_telem_route_wide(bool route_events_enabled, const char *tag)
{
    if (route_events_enabled) return true;

    // NULL or empty tag can never equal the (non-empty) configured TELEM
    // tag, so it's not a TELEM line -- route wide, same as any other tag.
    if (!tag || tag[0] == '\0') return true;

    // Exact match only -- a prefix/superstring like "TELEMETRY" is a
    // different tag and must still route wide.
    if (strcmp(tag, BB_LOG_TELEM_TAG_STR) == 0) return false;

    return true;
}

// Runtime routing state. Bridged from CONFIG_BB_LOG_TELEM_ROUTE_EVENTS
// (bool Kconfig, default n) the same way CONFIG_BB_LOG_UDP_SINK is consumed
// directly via #if elsewhere in bb_log -- a bool Kconfig symbol is safe to
// test with a bare #if (undefined reads as 0), no macro-shadow bridge
// needed the way an int/string Kconfig knob requires. Not mutex-protected:
// a plain volatile bool toggle, same pattern as s_udp_enabled in bb_log.c --
// s_log_vprintf only ever reads it, bb_log_telem_route_set() only ever
// writes it, and a torn read of a single bool is not a hazard here.
#ifdef ESP_PLATFORM
#if CONFIG_BB_LOG_TELEM_ROUTE_EVENTS
static volatile bool s_route_events_enabled = true;
#else
static volatile bool s_route_events_enabled = false;
#endif
#else
static volatile bool s_route_events_enabled = false;
#endif

void bb_log_telem_route_set(bool route_events_enabled)
{
    s_route_events_enabled = route_events_enabled;
}

bool bb_log_telem_route_get(void)
{
    return s_route_events_enabled;
}

bool bb_log_telem_should_route_wide(const char *line, size_t len)
{
    // Tag-only parse -- level_out and msg_out are optional (bb_log_line_parse
    // tolerates NULL for both), so the hot s_log_vprintf call site never pays
    // for the ~160-byte message copy just to make a routing decision.
    char tag[BB_LOG_TELEM_TAG_PARSE_CAP];
    bb_log_line_parse(line, len, NULL, tag, sizeof(tag), NULL, 0);
    return bb_log_telem_route_wide(s_route_events_enabled, tag);
}
