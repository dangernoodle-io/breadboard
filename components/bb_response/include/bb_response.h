// bb_response — reusable named-section registry.
//
// Each endpoint owns a file-scope bb_response_registry_t instead of a global
// singleton, so multiple endpoints can each have their own independent
// registry.
//
// Portable header — no esp_/cJSON/nvs_ includes.
#pragma once
#include "bb_core.h"
#include "bb_json.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_BB_RESPONSE_MAX
#define BB_RESPONSE_MAX 8
#else
#define BB_RESPONSE_MAX CONFIG_BB_RESPONSE_MAX
#endif

// Section callbacks.
// get_fn   writes fields into a new bb_json_t obj for the section.
// patch_fn reads fields from the parsed sub-object for the section.
//          NULL = read-only section; PATCH body with this key → BB_ERR_INVALID_ARG.
typedef void     (*bb_response_get_fn)  (bb_json_t section, void *ctx);
typedef bb_err_t (*bb_response_patch_fn)(bb_json_t section_patch, void *ctx);

typedef struct {
    const char           *name;
    bb_response_get_fn     get;
    bb_response_patch_fn   patch;
    void                 *ctx;
    const char           *schema_props; // JSON schema properties value for this section; may be NULL
} bb_response_entry_t;

typedef struct {
    bb_response_entry_t entries[BB_RESPONSE_MAX];
    int                count;
    uint8_t            cap;    // per-instance capacity limit; 0 → BB_RESPONSE_MAX; must not exceed BB_RESPONSE_MAX (clamped)
    bool               frozen;
    const char        *tag;  // log tag; may be NULL
} bb_response_registry_t;

// Register a named section.
// Returns BB_ERR_NO_SPACE when registry full, BB_ERR_INVALID_ARG on null name/get,
// BB_ERR_INVALID_STATE when frozen.
bb_err_t bb_response_register(bb_response_registry_t *reg,
                              const char *name,
                              bb_response_get_fn get,
                              bb_response_patch_fn patch,
                              void *ctx,
                              const char *schema_props);

// Build GET response: for each section, create a child obj, call get(child, ctx),
// then set root[name] = child.
void bb_response_build_get(const bb_response_registry_t *reg, bb_json_t root);

// Prevent further registrations.
void bb_response_freeze(bb_response_registry_t *reg);

// Assemble a composed object schema string:
//   {base_prefix}[,"<name>":<schema_props>]*{base_suffix}
// base_prefix is the opening of the properties object (e.g. '{"type":"object","properties":{').
// base_suffix closes it (e.g. '}}').
// Sections with NULL schema_props are omitted.
// Returns a heap-allocated string the caller must free, or NULL on OOM.
char *bb_response_assemble_schema(const bb_response_registry_t *reg,
                                 const char *base_prefix,
                                 const char *base_suffix);

// Freeze the registry then assemble its schema in one call.
// Logs a warning via bb_log_w if assembly returns NULL (OOM).
// Returns the heap-allocated schema string (caller must free), or NULL on OOM.
char *bb_response_freeze_and_assemble(bb_response_registry_t *reg, const char *base, const char *suffix);

#ifdef BB_RESPONSE_TESTING
// Override the malloc used by bb_response_assemble_schema (test-only).
void bb_response_set_malloc(void *(*m)(size_t));
#endif

#ifdef __cplusplus
}
#endif
