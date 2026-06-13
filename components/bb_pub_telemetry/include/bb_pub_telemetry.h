// bb_pub_telemetry — publisher status section provider for /api/telemetry.
//
// Registers a read-only "publisher" section via bb_telemetry_register_section.
// GET reports bb_pub runtime status (interval, counts, last publish state).
//
// Host twin: platform/host/bb_pub_telemetry/bb_pub_telemetry_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the "publisher" telemetry section (read-only).
// Called automatically when CONFIG_BB_PUB_TELEMETRY_AUTOREGISTER=y (PRE_HTTP tier).
bb_err_t bb_pub_telemetry_init(void);

#ifdef BB_PUB_TELEMETRY_TESTING

// Reset state for test isolation.
void bb_pub_telemetry_reset_for_test(void);

// Expose section get for direct test invocation.
void bb_pub_telemetry_section_get_for_test(bb_json_t section, void *ctx);

#endif /* BB_PUB_TELEMETRY_TESTING */

#ifdef __cplusplus
}
#endif
