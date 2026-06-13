// bb_http_pub_telemetry — HTTP publish section provider for /api/telemetry.
//
// Registers a "http" section via bb_telemetry_register_section.
// GET reads NVS "bb_http_pub" and reports configuration (masked TLS secrets).
// PATCH persists fields to NVS "bb_http_pub" and refreshes cached cfg.
//
// Host twin: platform/host/bb_http_pub_telemetry/bb_http_pub_telemetry_host.c
#pragma once
#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the "http" telemetry section.
// Called automatically when CONFIG_BB_HTTP_PUB_TELEMETRY_AUTOREGISTER=y (PRE_HTTP tier).
bb_err_t bb_http_pub_telemetry_init(void);

#ifdef BB_HTTP_PUB_TELEMETRY_TESTING

// Reset state for test isolation.
void bb_http_pub_telemetry_reset_for_test(void);

// Expose section get/patch for direct test invocation.
void     bb_http_pub_telemetry_section_get_for_test(bb_json_t section, void *ctx);
bb_err_t bb_http_pub_telemetry_section_patch_for_test(bb_json_t patch, void *ctx);

#endif /* BB_HTTP_PUB_TELEMETRY_TESTING */

#ifdef __cplusplus
}
#endif
