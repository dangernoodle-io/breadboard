// bb_log's TELEM wide-routing decision core -- pure, portable (no platform
// dependency), host-testable. See bb_log_internal.h for the full rationale.
// B1-831 PR-2: this file only builds the decision function + its Kconfig
// bridge -- nothing calls it yet. The s_log_vprintf call site (platform/
// espidf/bb_log/bb_log.c) lands in a later PR.
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
