#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_temp — satellite component that reads the SoC internal temperature sensor
 * and optionally registers a /api/health section.
 *
 * Include this component (REQUIRES bb_temp) when you want:
 *  - bb_temp_read_soc(): a portable wrapper around the SoC die-temperature
 *    sensor (ESP32-S2/S3/C3/C6/H2/... only; returns false on unsupported parts
 *    such as the classic ESP32 WROOM-32).
 *  - bb_temp_register_info(): to expose a "temp" section on /api/health with
 *    a schema contributed via bb_health_section_register (the composer-shaped
 *    seam, bb_health_section.h -- B1-1098, PR-3 of the bb_health/bb_response
 *    migration chain, epic B1-1054).
 *
 * ADDITIVE AND INERT (B1-1098): registering here populates only the NEW
 * bb_health_section table, which nothing renders yet -- the live /api/health
 * handler still assembles its response from the legacy
 * bb_health_register_section() registry (bb_health.h), untouched by this
 * component. The cutover that makes this section visible on the wire again
 * is a later PR (B1-1054 PR-5).
 *
 * Call bb_temp_register_info() before the section table is frozen.
 *
 * Presence of this satellite component in the build (via REQUIRES) is the
 * opt-in mechanism — no Kconfig gate is needed.
 */

#include <stdbool.h>
#include "bb_core.h"
#include "bb_health_section.h"
#include "bb_serialize.h"

/*
 * Read the SoC internal die temperature.
 * Returns true and writes *out_celsius when the sensor is supported and
 * the read succeeds; returns false otherwise (caller treats false as absent).
 * *out_celsius is untouched on false.
 */
bool bb_temp_read_soc(float *out_celsius);

// The "temp" /api/health section's snapshot: bool present + the SoC
// temperature (Celsius, rounded to one decimal -- same value-level rounding
// today's emitter applies) widened to double for BB_TYPE_F64 (bb_serialize_
// walk()'s F64 case always memcpy()s a fixed 8 bytes at the descriptor
// offset). soc_c's exact wire DIGIT formatting (fixed-6 vs
// shortest-round-trippable) is a render-level concern (B1-1102's
// f64_shortest flag), selected later by the composer at cutover (B1-1054
// PR-5) -- not this snapshot's concern.
typedef struct {
    bool   present;
    double soc_c;
} bb_temp_health_snap_t;

// Format-agnostic descriptor SSOT for bb_temp_health_snap_t. `soc_c` is
// omitted (present=false predicate) when the sensor read is absent --
// mirrors today's `{ "present": false }` (no "soc_c" key) shape.
extern const bb_serialize_desc_t bb_temp_health_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-2b-i-3) -- co-located JSON
// Schema docs/validation table for bb_temp_health_desc above, same
// #if-gated pattern as bb_wifi_http_wire_priv.h's exemplar (B1-1059 PR-2a).
// BB_SERIALIZE_META_HOST is a host-only define (set by the PlatformIO
// native env; see platformio.ini) -- NEVER set by the ESP-IDF/device
// build, so this declaration (and its definition in
// platform/{host,espidf}/bb_temp/bb_temp.c -- PLATFORM TWIN, kept
// byte-identical by convention like the rest of those twin files)
// compiles to nothing on-device.
//
// UNLIKE the REST/SSE clusters, this descriptor's hand-authored companion
// (k_temp_schema, in each platform twin) is a bare /api/health SECTION
// fragment -- no top-level "required"/"additionalProperties" of its own,
// because bb_health_section_t.schema_props is spliced verbatim into the
// /api/health composite's schema, which owns those decisions. See
// test_bb_temp_health_meta_golden.c (fragment-only assert,
// bb_serialize_meta_openapi_fragment()) for the fidelity proof this weaker
// check implies.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_temp_health_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// bb_health_fill_fn adapter: fills `dst` (a bb_temp_health_snap_t) from a
// live bb_temp_read_soc() call. `args` is unused (this section takes no
// query params). Returns BB_ERR_INVALID_ARG on NULL dst; the bb_health
// composer itself never passes NULL, so in composed use it always returns
// BB_OK (bb_health_section_register() validates snap_desc/fill at
// registration time, not per-call).
bb_err_t bb_temp_health_fill(void *dst, const bb_health_fill_args_t *args);

/*
 * Register a /api/health section named "temp" that emits:
 *   { "present": <bool> [, "soc_c": <number>] }
 * via bb_health_section_register() (bb_health_section.h). Also contributes
 * a hand-authored JSON-Schema fragment for this section's object.
 * Call before the section table is frozen.
 */
void bb_temp_register_info(void);

/**
 * Registry hook — calls bb_temp_register_info(). Takes no http_server
 * handle: bb_temp has no HTTP routes of its own, only a /api/health
 * section, so registering it needs no bb_http_server dependency.
 */
// bbtool:init tier=regular fn=bb_temp_autoregister_init
bb_err_t bb_temp_autoregister_init(void);

#ifdef __cplusplus
}
#endif
