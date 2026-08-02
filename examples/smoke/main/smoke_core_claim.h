#pragma once
// smoke_core_claim -- synthetic core-claimer proof harness (B1-1364 PR5
// validation). examples/smoke-only app code, never a library component --
// see smoke_core_claim.c for the full rationale.
//
// The `// bbtool:init` markers that wire these two entry points into the
// generated composition root live in bb_wire.h (the consumer manifest,
// examples/smoke/main/bb_wire.h), NOT here -- `bbtool codegen`'s component-
// header marker scan (collect_entries) only greps RESOLVED COMPONENTS'
// public headers (capability.smoke's closure), and this file is examples/
// smoke app code, not a components/<name>/include header. Only the single
// file named by `--consumer-manifest` (bb_wire.h) is grepped for
// app-level markers -- mirrors every other smoke_app.c-owned entry point
// already wired that way (see bb_wire.h's own header comment).
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the synthetic core-claimer task. No-op (returns BB_OK, no task
// created) when CONFIG_BB_SMOKE_CORE_CLAIM is off.
bb_err_t smoke_core_claim_start(void);

// Log httpd's actual worker-task core affinity (via FreeRTOS
// xTaskGetHandle("httpd") + xTaskGetCoreID()) -- the runtime, human-readable
// half of the proof. No-op (returns BB_OK) when CONFIG_BB_SMOKE_CORE_CLAIM
// is off.
bb_err_t smoke_core_claim_log_httpd_core(void);

#ifdef __cplusplus
}
#endif
