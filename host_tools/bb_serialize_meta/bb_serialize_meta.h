#pragma once

// Host-only "cold metadata" companion to a bb_serialize_desc_t (B1-767
// PR-7): validation constraints + JSON-Schema docs authored alongside a
// wire descriptor's field table, kept structurally SEPARATE from the hot
// bb_serialize_field_t walked at emit time. Lives entirely under
// host_tools/bb_serialize_meta/ -- neither the ESP-IDF component build nor
// bbtool's discovery.py component/board scan (components/ +
// platform/<plat>/ only) ever walks host_tools/, so this artifact is
// unreachable from any real-firmware build path by construction
// (composition, not a Kconfig/#if gate). NOT wired into any live handler or
// route registry; ADDITIVE only.
//
// A bb_serialize_desc_meta_t is keyed by field `key` against its paired
// bb_serialize_desc_t -- see bb_serialize_meta_validate() for the exact
// keying/agreement contract enforced between the two tables.

#include "bb_core.h"
#include "bb_serialize.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-field metadata row: validation constraints (schema-level contract)
// plus JSON-Schema documentation hints. Parallel in spirit to
// bb_serialize_field_t but intentionally NOT the same table -- this data is
// never walked at emit time.
typedef struct bb_serialize_field_meta_s {
    const char *key;  // MUST exactly match one bb_serialize_field_t.key in the paired desc

    // -- validation --
    bool     required;   // schema-level contract; DISTINCT from bb_serialize_field_t.present
                          // (a runtime emit predicate) -- no cross-check performed
    uint16_t min_len;     // string/array min length; 0 = unset. max_len is NOT duplicated
                           // here -- it stays on the base field for sizing/safety
    bool     has_min;
    bool     has_max;
    double   min;         // numeric lower bound; ignored unless has_min
    double   max;         // numeric upper bound; ignored unless has_max
    const char *const *enum_vals;  // NULL-terminated static string-literal array; NULL = no enum

    // -- docs --
    const char *title;         // NULL = omit
    const char *description;   // NULL = omit
    const char *format;        // JSON Schema "format"; NULL = omit
    const char *const *examples;  // NULL-terminated array of PRE-JSON-ENCODED literals
                                   // (e.g. "\"sntp\"", "0"); NULL = none

    // -- nesting (BB_TYPE_OBJ / BB_TYPE_ARR-of-OBJ only) --
    const struct bb_serialize_field_meta_s *children;  // parallel to base field's children; NULL for leaf
    uint16_t n_children;
} bb_serialize_field_meta_t;

// Descriptor-level metadata: one row table paired with one bb_serialize_desc_t.
typedef struct {
    const char                      *type_name;  // must match bb_serialize_desc_t.type_name
    const bb_serialize_field_meta_t *rows;
    uint16_t                         n_rows;
} bb_serialize_desc_meta_t;

// Validates that `meta` is a structurally-consistent companion to `desc`:
// type_name match, exactly one meta row per base field (no missing / no
// orphan rows), type/constraint agreement (enum_vals/min_len only for
// STR/STR_N/ARR-of-STR fields; min/max/has_min/has_max only for numeric
// fields), bounds sanity (has_min && has_max => min <= max; min_len <=
// base field.max_len), and recurses into `children` for BB_TYPE_OBJ /
// ARR-of-OBJ fields (bounded by BB_SERIALIZE_MAX_DEPTH). First-error
// semantics: stops at the first violation and writes a human-readable
// path+key reason into `err` (truncated, always NUL-terminated within
// `err_len`). Returns BB_OK if `meta` fully agrees with `desc`,
// BB_ERR_VALIDATION otherwise.
bb_err_t bb_serialize_meta_validate(const bb_serialize_desc_t      *desc,
                                     const bb_serialize_desc_meta_t *meta,
                                     char *err, size_t err_len);

// Composes a JSON Schema (draft 2020-12 subset) object fragment for
// `desc`/`meta` into `out` (capacity `out_size`): "type", "properties",
// "required", "items", "enum", "additionalProperties": false, plus
// "title"/"description"/"format"/"examples" wherever the paired meta row
// supplies them. Bounded-buffer, no heap -- same all-or-nothing overflow
// idiom as bb_serialize_json_render(): on success `*out_len` is the
// written length (excluding NUL) and `out` is NUL-terminated; on
// BB_ERR_NO_SPACE, `*out_len` is 0 and `out[0]` is '\0' -- never partial
// output. Does NOT itself validate `meta` against `desc` -- call
// bb_serialize_meta_validate() first if that guarantee is needed.
bb_err_t bb_serialize_meta_openapi_schema(const bb_serialize_desc_t      *desc,
                                           const bb_serialize_desc_meta_t *meta,
                                           char *out, size_t out_size, size_t *out_len);

#ifdef __cplusplus
}
#endif
