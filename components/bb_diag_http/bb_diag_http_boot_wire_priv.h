#pragma once

// bb_diag_http_boot_wire — private schema-compose seam for GET
// /api/diag/boot's REST response envelope (B1-1059 emit boot, B1-1189
// design). Distinct from components/bb_diag/bb_diag_boot_wire.c's
// "diag.boot" SSE topic-schema machinery (bb_diag_boot_get_schema()/
// bb_diag_boot_ensure_schema_patched()/s_diag_boot_schema_buf/
// k_diag_boot_schema) -- that composer serves the "diag.boot" bb_data key's
// bare-object topic schema (bb_diag_boot_ensure_schema_patched()); this one
// serves the REST route's {"ts_ms":<int>,"data":<...>} envelope shape
// (bb_diag_boot_render_envelope(), bb_diag_http_boot_wire.c). Same desc/
// meta pair (bb_diag_boot_wire_desc/bb_diag_boot_wire_meta), two entirely
// separate composers/buffers/symbol names -- see
// test_bb_diag_boot_wire_envelope_meta_golden.c for the envelope shape's
// byte-fidelity proof against platform/espidf/bb_diag_http/
// bb_diag_http_routes.c's hand-authored s_boot_get_responses[] literal.

#include "bb_diag_boot_wire.h"
#include "bb_serialize.h"
#include "bb_serialize_meta.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// GET /api/diag/boot RESPONSE schema (B1-1059 emit boot). A #define (not
// just the accessor below) so BOTH this component's config-OFF
// bb_diag_http_boot_wire_get_schema() (wire.c) AND platform/espidf/
// bb_diag_http/bb_diag_http_routes.c's static const s_boot_get_responses[]
// table (cross-TU, ESP-IDF-only -- can't call a function from a static
// initializer) can use the SAME literal text as a genuine compile-time
// constant expression -- mirrors BB_DIAG_PANIC_GET_SCHEMA_LITERAL's
// precedent (bb_diag_panic_get_wire_priv.h, B1-1059 emit batch C site C1).
// VERBATIM copy of the pre-existing hand literal (single source, no drift).
#define BB_DIAG_BOOT_GET_SCHEMA_LITERAL \
    "{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"ts_ms\":{\"type\":\"integer\"}," \
    "\"data\":{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"reset_reason\":{\"type\":\"string\"}," \
    "\"wdt_resets\":{\"type\":\"integer\"}," \
    "\"panic\":{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"available\":{\"type\":\"boolean\"}," \
    "\"boots_since\":{\"type\":\"integer\"}}," \
    "\"required\":[\"available\"]}," \
    "\"pending_verify\":{\"type\":\"boolean\"}," \
    "\"rolled_back\":{\"type\":\"boolean\"}," \
    "\"reboot_reason\":{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"source\":{\"type\":\"string\"}," \
    "\"detail\":{\"type\":\"string\"}," \
    "\"uptime_s\":{\"type\":\"integer\"}," \
    "\"epoch_s\":{\"type\":\"integer\"}," \
    "\"age_s\":{\"type\":\"integer\"}}," \
    "\"required\":[\"source\",\"uptime_s\",\"epoch_s\"]}," \
    "\"reboot_history\":{\"type\":\"array\",\"items\":{\"type\":\"object\"," \
    "\"properties\":{" \
    "\"source\":{\"type\":\"string\"}," \
    "\"epoch_s\":{\"type\":\"integer\"}," \
    "\"uptime_s\":{\"type\":\"integer\"}}," \
    "\"required\":[\"source\",\"epoch_s\",\"uptime_s\"]}}}," \
    "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\",\"pending_verify\",\"rolled_back\"," \
    "\"reboot_reason\",\"reboot_history\"]}}," \
    "\"required\":[\"ts_ms\",\"data\"]}"

// Composed-schema accessor pair (B1-1059 emit boot) -- cross-TU bridge: the
// register site (platform/espidf/bb_diag_http/bb_diag_http_routes.c) is
// ESP-IDF-only and cannot host a compose buffer or call the meta engine
// directly on host, so this portable TU owns the buffer/composer and
// exposes two accessors -- mirrors bb_diag_panic_get_wire_priv.h's site-C1
// exemplar. bb_diag_http_boot_wire_get_schema() is ALWAYS declared
// (zero config-OFF footprint beyond the accessor itself: it returns the
// pre-existing BB_DIAG_BOOT_GET_SCHEMA_LITERAL, unchanged);
// bb_diag_http_boot_wire_ensure_schema_patched() exists ONLY under
// CONFIG_BB_OPENAPI_RUNTIME_META.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_diag_http_boot_wire_ensure_schema_patched(void);

#ifdef BB_SERIALIZE_META_TESTING
// Test-only cap override -- see bb_diag_http_boot_wire.c's own doc comment
// (mirrors bb_health_schema_set_root_close_cap_for_test()'s precedent) for
// forcing this composer's own prefix/suffix bounds-check branches
// independent of the shared force_no_space seam.
void bb_diag_http_boot_wire_set_test_envelope_buf_cap(size_t cap);
#endif /* BB_SERIALIZE_META_TESTING */
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_diag_http_boot_wire_get_schema(void);

#ifdef __cplusplus
}
#endif
