// bb_pub_telemetry — publisher config + status section for /api/telemetry.
//
// Registers the "publisher" section via bb_telemetry_register_section.
// GET reports bb_pub runtime config (interval_ms, enabled) and status
// (counts, last publish state).
// PATCH accepts interval_ms and/or enabled; persists to NVS and applies live.
//
// Host twin: platform/host/bb_pub_telemetry/bb_pub_telemetry_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the "publisher" telemetry section (GET + PATCH).
// Called automatically when CONFIG_BB_PUB_TELEMETRY_AUTOREGISTER=y (PRE_HTTP tier).
bb_err_t bb_pub_telemetry_init(void);

#ifdef BB_PUB_TELEMETRY_TESTING

// Reset state for test isolation.
void bb_pub_telemetry_reset_for_test(void);

// Expose section get for direct test invocation.
void bb_pub_telemetry_section_get_for_test(bb_json_t section, void *ctx);

// Expose section patch for direct test invocation.
// Returns BB_OK on success, BB_ERR_INVALID_ARG on invalid values (bad interval).
bb_err_t bb_pub_telemetry_section_patch_for_test(bb_json_t patch, void *ctx);

#endif /* BB_PUB_TELEMETRY_TESTING */

#ifdef __cplusplus
}
#endif
