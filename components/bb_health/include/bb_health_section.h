#pragma once

/**
 * @brief Composer-shaped section registry for the future GET /api/health
 * document (B1-1096, PR-1 of 6 in the bb_health/bb_response migration chain
 * -- see epic B1-1054). Unlike bb_diag_section.h's ROUTER (one section
 * served per GET /api/diag/<name> request), this registry is a COMPOSER:
 * every registered section is rendered as a child object of ONE document in
 * a single request -- mirrors today's bb_health_register_section() /
 * bb_response_registry_t (bb_health.h / bb_response.h), just re-shaped onto
 * bb_serialize_desc_t snapshots instead of hand-rolled bb_json_t builder
 * callbacks.
 *
 * ADDITIVE AND INERT: nothing in this PR consumes this registry. The live
 * /api/health handler (platform/espidf/bb_health/bb_health.c) is untouched
 * and keeps using the legacy bb_health_register_section() API (bb_health.h)
 * for now. This is a NEW, separate file/API -- deliberately not folded into
 * bb_health.h -- so the legacy one can be deleted cleanly once every
 * producer has moved over (B1-1054 PR-5/6).
 */

#include "bb_core.h"
#include "bb_serialize.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed table capacity -- matches today's BB_RESPONSE_MAX (bb_response.h),
// the legacy /api/health section registry's own cap.
#ifdef CONFIG_BB_HEALTH_SECTION_TABLE_CAP
#define BB_HEALTH_SECTION_TABLE_CAP CONFIG_BB_HEALTH_SECTION_TABLE_CAP
#else
#define BB_HEALTH_SECTION_TABLE_CAP 8
#endif

// Max length (including the terminating NUL) of a section name -- the wire
// key a section is composed under (e.g. "mqtt"). bb_health_section_register()
// rejects (BB_ERR_INVALID_ARG) any name whose strlen() is >= this bound
// rather than silently truncating it.
#define BB_HEALTH_SECTION_NAME_MAX 24

// Args passed to a section's fill hook on every request. bb_health-LOCAL
// type (deliberately NOT bb_diag_fill_args_t or bb_data_gather_args_t) --
// GET /api/health takes no query params, so unlike bb_diag_fill_args_t
// there is no `query` field here.
typedef struct {
    void *ctx;
} bb_health_fill_args_t;

// Fill hook: fills `dst` (the section's snap_desc's snap_size bytes,
// CALLER-OWNED scratch storage) from live sources. `args` is never NULL.
typedef bb_err_t (*bb_health_fill_fn)(void *dst, const bb_health_fill_args_t *args);

// One section's registration. `snap_desc` is BORROWED -- the caller (the
// producer component) keeps it alive for the process lifetime, typically a
// static const. `name`/`schema_props` strings must likewise outlive
// registration (static const/string literals).
typedef struct {
    const char                *name;         // wire key, e.g. "mqtt"; strlen() < BB_HEALTH_SECTION_NAME_MAX
    const bb_serialize_desc_t *snap_desc;    // BORROWED SSOT
    bb_health_fill_fn          fill;
    void                      *ctx;
    const char                *schema_props; // hand-authored JSON-Schema fragment for this section's
                                              // object; must have static/rodata lifetime. NULL -> no
                                              // schema contribution (mirrors bb_response_entry_t).
} bb_health_section_t;

// Registers `section`. Copies its fields (not the pointer) into the
// registry's own table -- `section` itself may be a stack temporary, but
// every pointer field it carries (name/snap_desc/fill/ctx/schema_props)
// must remain valid for the process lifetime.
//
// First-registration wins: a duplicate `name` is REJECTED
// (BB_ERR_INVALID_STATE), never silently overridden -- mirrors
// bb_diag_register_section()'s precedent (two producers claiming the same
// section name is a composition bug).
//
// Returns BB_ERR_INVALID_ARG if `section`, `section->name`,
// `section->snap_desc`, or `section->fill` is NULL, or if
// strlen(section->name) >= BB_HEALTH_SECTION_NAME_MAX.
// Returns BB_ERR_INVALID_STATE if `section->name` is already registered, or
// if called after bb_health_section_freeze() (server started).
// Returns BB_ERR_NO_SPACE if the table is full (BB_HEALTH_SECTION_TABLE_CAP
// distinct names already registered), or if section->snap_desc->snap_size
// exceeds BB_HEALTH_SECTION_SCRATCH_BYTES (the shared per-section render
// scratch the future composer fills each section into) -- a loud,
// attach-time reject rather than a silent per-request truncation.
bb_err_t bb_health_section_register(const bb_health_section_t *section);

// Freezes the registry -- every subsequent bb_health_section_register()
// call returns BB_ERR_INVALID_STATE. Idempotent. Not yet called from any
// production code path in this PR (the future composer's server-init hook
// will call this the way bb_health_init() calls
// bb_response_freeze_and_assemble() today, B1-1054 PR-5); exposed now so
// this PR's own tests can exercise the reject-after-freeze contract.
void bb_health_section_freeze(void);

#ifdef BB_HEALTH_SECTION_TESTING
// Test-only: clears every registered section and un-freezes the registry.
void bb_health_section_test_reset(void);

// Test-only: looks up a registered section's stored (by-value copy) by
// name, for asserting the registry's copy-in contract (e.g. schema_props
// round-trips the caller's pointer). Returns NULL if `name` is NULL or not
// registered.
const bb_health_section_t *bb_health_section_test_find(const char *name);
#endif

#ifdef __cplusplus
}
#endif
