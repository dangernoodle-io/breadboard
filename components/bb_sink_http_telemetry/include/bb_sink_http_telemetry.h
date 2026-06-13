// bb_sink_http_telemetry — HTTP publish section provider for /api/telemetry.
//
// Registers a "http" section via bb_telemetry_register_section.
// GET reads NVS "bb_sink_http" and reports configuration (masked TLS secrets).
// PATCH persists fields to NVS "bb_sink_http" and refreshes cached cfg.
//
// Host twin: platform/host/bb_sink_http_telemetry/bb_sink_http_telemetry_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the "http" telemetry section.
// Called automatically when CONFIG_BB_SINK_HTTP_TELEMETRY_AUTOREGISTER=y (PRE_HTTP tier).
bb_err_t bb_sink_http_telemetry_init(void);

#ifdef BB_SINK_HTTP_TELEMETRY_TESTING

// Reset state for test isolation.
void bb_sink_http_telemetry_reset_for_test(void);

// Expose section get/patch for direct test invocation.
void     bb_sink_http_telemetry_section_get_for_test(bb_json_t section, void *ctx);
bb_err_t bb_sink_http_telemetry_section_patch_for_test(bb_json_t patch, void *ctx);

#endif /* BB_SINK_HTTP_TELEMETRY_TESTING */

#ifdef __cplusplus
}
#endif
