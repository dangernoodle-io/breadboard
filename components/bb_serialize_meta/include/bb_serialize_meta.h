#pragma once

/**
 * @brief Device-shippable "cold metadata" companion engine for
 * bb_serialize_desc_t (B1-767 PR-7, B1-1059): validation constraints +
 * JSON-Schema docs for a future runtime OpenAPI schema generator, gated off
 * by default.
 */

// "Cold metadata" companion to a bb_serialize_desc_t (B1-767 PR-7):
// validation constraints + JSON-Schema docs authored alongside a wire
// descriptor's field table, kept structurally SEPARATE from the hot
// bb_serialize_field_t walked at emit time. Originally host-only, unreachable
// from any real-firmware build path by construction (lived under
// host_tools/bb_serialize_meta/, outside bbtool's discovery.py component/
// board scan). Relocated into this real ESP-IDF component (device-shippable
// PR, B1-1059) so it CAN compile into firmware, gated OFF by default via
// BB_SERIALIZE_META_SHIP below -- still NOT wired into any live handler or
// route registry in this PR; ADDITIVE only.
//
// A bb_serialize_desc_meta_t is keyed by field `key` against its paired
// bb_serialize_desc_t -- see bb_serialize_meta_validate() for the exact
// keying/agreement contract enforced between the two tables.

#include "bb_core.h"
#include "bb_serialize.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Ship gate for every co-located bb_serialize_desc_meta_t table in
// components/<name>/ (guarded on this macro, not on BB_SERIALIZE_META_HOST
// directly): defined whenever the PlatformIO native host env sets
// BB_SERIALIZE_META_HOST=1 (unconditional on host, unchanged from this
// engine's host_tools-era behavior) OR CONFIG_BB_OPENAPI_RUNTIME_META is
// set (components/bb_openapi/Kconfig, default n -- OFF ships zero bytes of
// meta tables/engine on-device; ON compiles them in for the future runtime
// OpenAPI schema generator, not wired up yet in this PR).
#if defined(BB_SERIALIZE_META_HOST) || defined(CONFIG_BB_OPENAPI_RUNTIME_META)
#define BB_SERIALIZE_META_SHIP 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

// A meta row documents either a single field ("FIELD", the default -- every
// existing table is unambiguously this kind since the struct's 0-value
// default for `.kind` IS BB_SERIALIZE_META_KIND_FIELD) or a duplicate-`.key`
// group rendered as a JSON Schema "oneOf" ("ONEOF", B1-1181a). See
// bb_serialize_field_meta_s's "duplicate-key" doc below for the full
// contract.
typedef enum {
    BB_SERIALIZE_META_KIND_FIELD = 0,
    BB_SERIALIZE_META_KIND_ONEOF = 1,
} bb_serialize_meta_kind_t;

// Per-field metadata row: validation constraints (schema-level contract)
// plus JSON-Schema documentation hints. Parallel in spirit to
// bb_serialize_field_t but intentionally NOT the same table -- this data is
// never walked at emit time.
typedef struct bb_serialize_field_meta_s {
    const char *key;  // MUST exactly match one bb_serialize_field_t.key in the paired desc

    // -- validation --
    bool     required;   // schema-level contract; DISTINCT from bb_serialize_field_t.present
                          // (a runtime emit predicate) -- no cross-check performed
    uint16_t min_len;     // string/array min length; 0 = unset.
    uint16_t max_len;     // string/array max length (schema "maxLength"); 0 = unset.
                          // DISTINCT from the base bb_serialize_field_t.max_len, which
                          // sizes the wire buffer/truncation safety cap and may be
                          // deliberately LARGER than the true validation bound (e.g.
                          // bb_wifi_http_creds_wire_t oversizes ssid/pass so an
                          // overlong value is detectable rather than silently
                          // truncated) -- this field carries the real schema-level
                          // bound. Enforced by bb_serialize_meta_validate() to never
                          // exceed the base field's max_len.
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

    // -- duplicate-key ("oneOf" / occurrence-tagged) rows, B1-1181a --
    //
    // bb_serialize.h's populate contract explicitly supports binding two+
    // physical bb_serialize_field_t rows to the SAME wire `.key` (see
    // bb_system_routes.c's s_reboot_fields / bb_storage_http_routes.c's
    // s_storage_delete_fields). A duplicate-key group needs exactly ONE
    // meta row (never one row per physical occurrence -- that would
    // re-trigger the "duplicate meta row" bijection error), tagged one of
    // two ways:
    //
    //   - `kind == BB_SERIALIZE_META_KIND_FIELD` (the default) with
    //     `.occurrence` set to the 0-based index (in physical field order)
    //     of the ONE occurrence this row documents -- every OTHER physical
    //     occurrence of the key is intentionally left doc-invisible (no
    //     row, no rendered schema for it). This is the reboot "ts" shape:
    //     the wire binds "ts" as BOTH U64 (occurrence 0, the real value)
    //     and F64 (occurrence 1, an internal divergence-guard shadow never
    //     meant to surface) -- the meta table carries ONE row tagging
    //     occurrence 0, and the F64 occurrence has no row at all.
    //   - `kind == BB_SERIALIZE_META_KIND_ONEOF` with `.branches` a
    //     NULL-terminated array of `n_branches` per-occurrence rows
    //     (`.branches[i]` documents the i-th physical occurrence, in field
    //     order) and `n_branches` equal to the key's total physical
    //     occurrence count. This renders `"<key>":{"oneOf":[<branch
    //     schemas>...]}` -- the storage_delete "namespace" shape (bound
    //     once as STR, once as ARR-of-STR). Both the engine's occurrence
    //     helpers below and its ONEOF-branch matching assume every physical
    //     occurrence of a duplicate `.key` appears CONTIGUOUSLY in the
    //     descriptor's `fields[]` array (true of every duplicate-key table
    //     in this repo) -- an interleaved duplicate key is unsupported.
    //     Docs (title/description/format/examples/enum_vals) and
    //     `.required` attach to the ONEOF row itself, never to an
    //     individual branch -- a branch row's own docs/required fields are
    //     ignored by the composer.
    //
    // A duplicate key with NO meta row at all is REJECTED by
    // bb_serialize_meta_validate() -- an accidental copy-paste omission
    // must still fail, not silently pass through the relaxed bijection (a
    // deliberate FIELD row IS valid at `.occurrence == 0`, its default --
    // that's the reboot "ts" row's own shape).
    bb_serialize_meta_kind_t kind;
    uint16_t                 occurrence;  // BB_SERIALIZE_META_KIND_FIELD only; 0-based
    const struct bb_serialize_field_meta_s *const *branches;  // BB_SERIALIZE_META_KIND_ONEOF only;
                                                                // NULL-terminated
    uint16_t n_branches;                                       // BB_SERIALIZE_META_KIND_ONEOF only
} bb_serialize_field_meta_t;

// Number of physical `fields[]` entries (0-based index < `n_fields`) sharing
// `fields[i].key` that appear at or before index `i` -- e.g. 0 for the
// first physical occurrence of a key, 1 for the second, etc. Shared by
// bb_serialize_meta_openapi.c (occurrence-aware top-level field loop) and
// bb_serialize_meta_validate.c (duplicate-key row matching); trivial/cheap
// enough (bounded by a descriptor's small, fixed `n_fields`) that a
// static-inline header helper is preferable to exporting two extra
// non-static symbols from this host-only engine.
static inline uint16_t bb_serialize_meta_occurrence_index(const bb_serialize_field_t *fields,
                                                           uint16_t i)
{
    uint16_t idx = 0;
    for (uint16_t j = 0; j < i; j++) {
        if (strcmp(fields[j].key, fields[i].key) == 0) idx++;
    }
    return idx;
}

// Total number of `fields[]` entries (of `n_fields`) whose `.key` equals
// `key` -- 1 for an ordinary, non-duplicated key.
static inline uint16_t bb_serialize_meta_occurrence_count(const bb_serialize_field_t *fields,
                                                           uint16_t n_fields, const char *key)
{
    uint16_t count = 0;
    for (uint16_t j = 0; j < n_fields; j++) {
        if (strcmp(fields[j].key, key) == 0) count++;
    }
    return count;
}

// Descriptor-level metadata: one row table paired with one bb_serialize_desc_t.
typedef struct {
    const char                      *type_name;  // must match bb_serialize_desc_t.type_name
    const bb_serialize_field_meta_t *rows;
    uint16_t                         n_rows;
} bb_serialize_desc_meta_t;

// Validates that `meta` is a structurally-consistent companion to `desc`:
// type_name match, exactly one meta row per base field (no missing / no
// orphan rows), type/constraint agreement (enum_vals/min_len/max_len only
// for STR/STR_N/ARR-of-STR fields; min/max/has_min/has_max only for
// numeric fields), bounds sanity (has_min && has_max => min <= max;
// min_len <= max_len; min_len/max_len <= base field.max_len), and
// recurses into `children` for BB_TYPE_OBJ /
// ARR-of-OBJ fields (bounded by BB_SERIALIZE_MAX_DEPTH). First-error
// semantics: stops at the first violation and writes a human-readable
// path+key reason into `err` (truncated, always NUL-terminated within
// `err_len`). Returns BB_OK if `meta` fully agrees with `desc`,
// BB_ERR_VALIDATION otherwise.
//
// Duplicate-`.key` field groups (B1-1181a, see bb_serialize_field_meta_s's
// "duplicate-key" doc above) relax the one-row-per-PHYSICAL-field bijection
// to one-row-per-KEY, opt-in per key: a key with occurrence_count > 1 is
// valid iff its single meta row is `kind == BB_SERIALIZE_META_KIND_ONEOF`
// with `n_branches == occurrence_count` (every physical occurrence
// documented, in order, by one branch), OR `kind ==
// BB_SERIALIZE_META_KIND_FIELD` with `.occurrence` a valid index (<
// occurrence_count) -- documenting exactly that one occurrence, the rest
// left undocumented. A duplicate key with NO row at all, or a plain FIELD
// row whose `.occurrence` doesn't disambiguate which physical occurrence it
// documents, is still rejected (an accidental copy-paste duplicate must
// still fail). Type/constraint agreement for a ONEOF row is checked
// per-branch, against the correspondingly-ordered physical field.
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
