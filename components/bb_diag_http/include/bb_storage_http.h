#pragma once

/**
 * @brief Backend-agnostic DELETE /api/diag/storage route over the
 * bb_storage facade, plus POST /api/diag/factory-reset (whole-partition
 * erase + reboot) — works for any registered backend (nvs today,
 * ram/rtc/a future sdcard) with no new route component per backend.
 */

// bb_storage_http — backend-agnostic DELETE route for bb_storage (B1-757).
//
// B1-1154 (KB 1477): the bb_storage_http COMPONENT was dissolved -- these
// two routes now live under bb_diag_http (an SSOT component owns no routes;
// both routes were already diag-namespaced). This header/its .c files moved
// verbatim; the CONFIG_BB_STORAGE_HTTP_FACTORY_RESET Kconfig symbol below
// KEEPS its bb_storage_http-era name unchanged.
//
// Registers DELETE /api/diag/storage on the shared bb_http_server. Rehomed
// from the old DELETE /api/nvs (platform/espidf/bb_nv/bb_nv_delete_routes.c),
// which was welded to the "nvs" backend and required bb_diag to reach into
// bb_nv's PRIVATE include dir (target_include_directories(... PRIVATE
// .../bb_nv)) just to call bb_nv_delete_routes_init — a build-system-level
// component-boundary violation.
//
// This component sits on the bb_storage FACADE only — REQUIRES bb_core
// bb_storage, never bb_storage_nvs — so any backend registered with
// bb_storage (nvs today, ram/rtc/a future sdcard) works through this SAME
// route with no new route component per backend.
//
// Body schema:
//   {
//     "backend":   <string>,                      // optional, default "nvs"
//     "namespace": <string | array-of-strings>,    // required
//     "key":       <string>,                       // optional; forbidden with array namespace
//     "confirm":   true,                           // required; else 412
//     "wipe_wifi": true                             // required when namespace is/includes the
//                                                    // wifi-creds namespace bb_settings owns
//   }
//
// Guard policy:
//   - "confirm": true required (412 without it).
//   - a namespace bb_settings identifies as the wifi-creds namespace (see
//     bb_settings_ns_is_wifi_creds) additionally requires "wipe_wifi": true,
//     because that namespace holds wifi credentials. Note: if
//     CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP is enabled, credentials are
//     automatically restored from the RTC mirror on the next boot if the
//     mirror is valid.
//   - "key" with an array namespace returns 400 (ambiguous).
//   - a backend that does not implement namespace-level erase
//     (bb_storage_vtable_t.erase_namespace == NULL) surfaces
//     BB_ERR_UNSUPPORTED as 501 — never a silent no-op on a destructive
//     request.
//
// Responses:
//   200 {"deleted": [...]}  — list of namespaces (and key) cleared
//   400 missing/invalid namespace, or key+array namespace combo
//   412 missing confirm / missing wipe_wifi when the wifi-creds namespace is in scope
//   500 storage erase operation failed
//   501 backend does not support namespace-level erase

#include "bb_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registry hook — registers DELETE /api/diag/storage.
// bbtool:init tier=regular fn=bb_storage_http_routes_init server=true
bb_err_t bb_storage_http_routes_init(bb_http_handle_t server);

// Registry hook — registers POST /api/diag/factory-reset. Rehomed off bb_nv
// (B1-960, bb_nv dissolution epic B1-708) — was POST /api/factory-reset on
// bb_nv_factory_reset_routes_init. A valid request body
// {"confirm":"factory-reset"} erases the whole "nvs" bb_storage backend
// (bb_storage_erase_all), invalidates the RTC creds mirror (when
// CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP is enabled), responds 202, then reboots
// after ~500 ms. A SEPARATE // bbtool:init marker from
// bb_storage_http_routes_init above, 1:1 with this one route — not folded
// into a shared multi-route init.
//
// Gated behind CONFIG_BB_STORAGE_HTTP_FACTORY_RESET (default n — destructive,
// opt-in only; same posture the deleted bb_nv route had via
// CONFIG_BB_NV_FACTORY_RESET). codegen's `// bbtool:init` marker scan has no
// preprocessor awareness (grep-time, see wire_parse.py), so bb_app_init.c
// unconditionally calls this fn regardless of the Kconfig value; the #else
// branch below supplies a no-op stub with matching signature so that call
// always links, mirroring bb_nv.h's bb_nv_factory_reset_routes_init stub
// pattern.
#if defined(CONFIG_BB_STORAGE_HTTP_FACTORY_RESET) && CONFIG_BB_STORAGE_HTTP_FACTORY_RESET
// bbtool:init tier=regular fn=bb_storage_http_factory_reset_routes_init server=true
bb_err_t bb_storage_http_factory_reset_routes_init(bb_http_handle_t server);
#else
static inline bb_err_t bb_storage_http_factory_reset_routes_init(bb_http_handle_t server)
{
    (void)server;
    return BB_OK;
}
#endif /* defined(CONFIG_BB_STORAGE_HTTP_FACTORY_RESET) && CONFIG_BB_STORAGE_HTTP_FACTORY_RESET */

#ifdef BB_STORAGE_HTTP_TESTING
// Expose the handlers for host unit tests.
bb_err_t bb_storage_http_delete_handler_for_test(bb_http_request_t *req);
// Binds the "storage_delete" bb_data key (production gather/apply hooks)
// without requiring a real bb_http_handle_t server -- see the .c file's own
// doc comment.
bb_err_t bb_storage_http_delete_bind_for_test(void);
#if defined(CONFIG_BB_STORAGE_HTTP_FACTORY_RESET) && CONFIG_BB_STORAGE_HTTP_FACTORY_RESET
bb_err_t bb_storage_http_factory_reset_handler_for_test(bb_http_request_t *req);
// Binds the "factory_reset" bb_data key (production gather/apply hooks)
// without requiring a real bb_http_handle_t server -- see the .c file's own
// doc comment.
bb_err_t bb_storage_http_factory_reset_bind_for_test(void);

// bb_serialize_desc_meta_t companion (B1-1059 PR-2b-i-1) -- co-located JSON
// Schema docs/validation table for the POST /api/diag/factory-reset request
// descriptor (bb_storage_http_routes.c's file-scope s_factory_reset_desc),
// same #if-gated pattern as bb_wifi_http_wire_priv.h's exemplar (B1-1059
// PR-2a). BB_SERIALIZE_META_HOST is a host-only define (set by the
// PlatformIO native env; see platformio.ini) -- NEVER set by the ESP-IDF/
// device build, so these two declarations (and their definitions in
// bb_storage_http_routes.c) compile to nothing on-device. The desc itself
// is file-scope static (unlike the wire-descriptor precedent, this route
// has no companion _wire_priv.h), so a for-test accessor exposes it rather
// than an extern -- same "_for_test" naming convention as the
// BB_STORAGE_HTTP_TESTING-gated fns above.
#if defined(BB_SERIALIZE_META_HOST)
#include "bb_serialize_meta.h"

const bb_serialize_desc_t *bb_storage_http_factory_reset_desc_for_test(void);
extern const bb_serialize_desc_meta_t bb_storage_http_factory_reset_meta;
#endif /* BB_SERIALIZE_META_HOST */
#endif /* defined(CONFIG_BB_STORAGE_HTTP_FACTORY_RESET) && CONFIG_BB_STORAGE_HTTP_FACTORY_RESET */
#endif /* BB_STORAGE_HTTP_TESTING */

#ifdef __cplusplus
}
#endif
