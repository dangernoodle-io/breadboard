// bb_log's TELEM wide-routing decision core -- pure, portable (no platform
// dependency), host-testable. See bb_log_internal.h for the full rationale.
// B1-831 PR-2 built the decision function (bb_log_telem_route_wide) + its
// Kconfig bridge. B1-831 PR-3 added the runtime enable/disable state plus a
// now-deleted line-level combinator (bb_log_telem_should_route_wide) that
// parsed a tag out of the raw console line for the s_log_vprintf call site.
// B1-1443 PR-2 removed that combinator along with bb_log_line_parse(): our
// own bb_log_e/w/i/d/v lines call bb_log_telem_route_wide() directly with
// their real tag (bb_log_emit(), platform/espidf/bb_log/bb_log.c) -- no
// parse needed -- and foreign/vendored ESP_LOGx lines are wrapped opaque
// with no tag, so they always route wide (see bb_log_internal.h).
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
