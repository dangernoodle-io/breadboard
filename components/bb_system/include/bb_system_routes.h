#pragma once

/**
 * @brief POST /api/reboot route over bb_system, carved out of bb_system.h so
 * a codegen consumer can compose bb_system's primitives (reboot-reason SSOT,
 * boot banner, boot-fail counters, bb_system_snap) without inheriting the
 * HTTP route surface / the bb_http_server edge (B1-1313).
 */

// bb_system_routes -- co-located with bb_system per the "opt-in *_routes
// endpoint co-located with its data SSOT" convention (see bb_storage_http.h/
// bb_log_http.h). Only the declarations moved here (B1-1313); the routes'
// definitions stay in bb_system_routes.c (platform/espidf/bb_system/
// bb_system_routes.c, portable, host-compiled via the bbtool-scaffold-hint
// in components/bb_system/CMakeLists.txt) -- NOT in scope, see B1-1313.
// The bbtool init marker for bb_system_routes_init does NOT live in this
// header -- it was relocated to the consuming composition's manifest
// (examples/smoke/main/bb_wire.h) under B1-1279/B1-1314/B1-1315: a
// component composing bb_system for its reboot-reason SSOT (etc.) must not
// be forced to also expose POST /api/reboot, so the route-registering hook
// is opt-in per consumer rather than auto-wired from this component header.
//
// bb_http_server.h is itself fully portable (opaque bb_http_handle_t, no
// ESP_PLATFORM-gated content), so this include and the declarations below
// carry no platform leak.

#include "bb_core.h"
#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hand-authored JSON Schema for POST /api/reboot's request body (B1-1059
// emit batch A, site 2) -- bb_system_routes.c is portable (compiled on both
// ESP-IDF and host), so this is unconditional, not ESP_PLATFORM-gated. See
// test/test_host/test_bb_system_reboot_meta_golden.c for the byte-fidelity
// proof against bb_system_reboot_meta.
extern const char *const bb_system_reboot_request_schema;

/// Registry hook — registers POST /api/reboot.
bb_err_t bb_system_routes_init(bb_http_handle_t server);

#ifdef BB_SYSTEM_TESTING
/// Expose the POST /api/reboot handler for host unit tests (B1-1148 PR1).
/// req is the opaque bb_http_request_t handle (no ESP_PLATFORM dependency --
/// same posture as bb_storage_http_delete_handler_for_test).
bb_err_t bb_system_reboot_handler_for_test(bb_http_request_t *req);

/// Test-only (B1-1148 PR2): binds the "reboot" bb_data key against the
/// production gather/apply hooks without going through bb_system_routes_init()
/// (which additionally requires a real bb_http_handle_t server -- unavailable
/// in host tests). Call after bb_data_test_reset() and before driving
/// bb_system_reboot_handler_for_test(). Mirrors
/// bb_storage_http_factory_reset_bind_for_test().
bb_err_t bb_system_reboot_bind_for_test(void);

/// Runtime-compose test accessors (B1-1059 emit batch A, site 2) for POST
/// /api/reboot's request schema (bb_system_routes.c's file-scope
/// s_reboot_route). The assemble fn runs the same guarded, idempotent
/// compose-and-patch step bb_system_routes_init() runs
/// (CONFIG_BB_OPENAPI_RUNTIME_META build only; a documented no-op returning
/// BB_OK otherwise). The get fn returns the route's current request_schema
/// pointer (NULL before assemble has run, config-ON only). Mirrors
/// bb_diag_storage_nvs.c's pilot accessors.
bb_err_t bb_system_reboot_assemble_request_schema_for_test(void);
const char *bb_system_reboot_get_request_schema_for_test(void);

// bb_serialize_desc_meta_t companion (B1-1181a) -- co-located JSON Schema
// docs/validation table for the POST /api/reboot request descriptor
// (bb_system_routes.c's file-scope s_reboot_desc), same #if-gated pattern
// as bb_storage_http.h's bb_storage_http_factory_reset_meta (B1-1059
// PR-2b-i-1). BB_SERIALIZE_META_HOST is a host-only define (set by the
// PlatformIO native env; see platformio.ini) -- NEVER set by the ESP-IDF/
// device build, so these two declarations (and their definitions in
// bb_system_routes.c) compile to nothing on-device. The desc itself is
// file-scope static, so a for-test accessor exposes it rather than an
// extern -- same "_for_test" naming convention as the BB_SYSTEM_TESTING-
// gated fns above.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

const bb_serialize_desc_t *bb_system_reboot_desc_for_test(void);
extern const bb_serialize_desc_meta_t bb_system_reboot_meta;
#endif /* BB_SERIALIZE_META_SHIP */
#endif /* BB_SYSTEM_TESTING */

#ifdef __cplusplus
}
#endif
