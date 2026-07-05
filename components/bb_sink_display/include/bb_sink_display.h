#pragma once

// bb_sink_display — northstar display-egress sink (v1).
//
// Consumes bb_filter (over bb_attrs-tagged fields) + a hand-authored static
// bb_sink_display_field_t array (v1; see the phasing note below) +
// bb_cache_reactive's on_change/on_register/on_remove triad, and renders the
// selected subset either via bb_display's DEFAULT_LINES text rows or a
// caller-owned CUSTOM render_fn.
//
// bb_field_t is DISPLAY-OWNED for v1 -- no separate bb_field component
// exists. bb_sink_display_field_t embeds a bb_attrs_t so the same field
// array is directly bb_filter-selectable (kind_mask/priority/tag gating).
//
// "Inject the abstraction, not the driver": v1 has exactly two injection
// points, neither of which is a bb_display handle (bb_display has no
// instantiable type -- it is a link-time-selected singleton, chosen by
// probe-and-select over whichever bb_display_<chip> backend the board
// links):
//   - DEFAULT_LINES policy calls the bb_display singleton's free functions
//     (bb_display_draw_text/_width/_height) directly -- the board's choice
//     of backend is the injection, resolved at link time, never a runtime
//     handle.
//   - CUSTOM policy hands the selected+formatted row subset to
//     cfg->custom -- the app owns its own rich UI (e.g. LVGL/lv_subject
//     bridge) and never touches bb_display. THIS is the rich-UI injection
//     point.
// bb_sink_display itself NEVER links LVGL.
//
// PHASING (v1, locked): the field catalog is a hand-authored
// `static const bb_sink_display_field_t[]` owned by the platform/espidf
// glue file (platform/espidf/bb_sink_display/bb_sink_display.c). A future
// Lane-3 swap replaces that array with a bb_collection-fed satellite
// contribution behind a one-line #if -- the descriptor shape (this header)
// does not change.
//
// Multi-display seam (reserved, not built): bb_sink_display_config_t.display
// is NULL in every real v1 consumer (single-display boards only). A non-NULL
// value is a reserved future extension point (see the field's own doc
// comment) and is rejected today with BB_ERR_UNSUPPORTED.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "bb_core.h"
#include "bb_attrs.h"
#include "bb_filter.h"
#include "bb_json.h"

// ---------------------------------------------------------------------------
// Kconfig bridge (pattern from bb_clock.h / CLAUDE.md)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_SINK_DISPLAY_MAX_FIELDS
#define BB_SINK_DISPLAY_MAX_FIELDS CONFIG_BB_SINK_DISPLAY_MAX_FIELDS
#endif
#ifdef CONFIG_BB_SINK_DISPLAY_VALUE_MAX
#define BB_SINK_DISPLAY_VALUE_MAX CONFIG_BB_SINK_DISPLAY_VALUE_MAX
#endif
#endif
#ifndef BB_SINK_DISPLAY_MAX_FIELDS
#define BB_SINK_DISPLAY_MAX_FIELDS 16
#endif
#ifndef BB_SINK_DISPLAY_VALUE_MAX
#define BB_SINK_DISPLAY_VALUE_MAX 40
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Field descriptor (display-owned in v1 -- see phasing note above)
// ---------------------------------------------------------------------------

typedef enum {
    BB_SINK_DISPLAY_KIND_INT = 0,
    BB_SINK_DISPLAY_KIND_FLOAT,
    BB_SINK_DISPLAY_KIND_BOOL,
    BB_SINK_DISPLAY_KIND_STRING,
} bb_sink_display_kind_t;

typedef struct bb_sink_display_field_s bb_sink_display_field_t;

// Format `value_item` (the parsed json_path value; caller-invoked with the
// field's own kind already known) into `out` ("label: value unit" for the
// default formatter). `value_item` is a bb_json_t owned by the resolver's
// call frame -- valid only for the duration of the call, never retained.
// Writes at most out_cap-1 bytes + NUL.
typedef void (*bb_sink_display_format_fn)(const bb_sink_display_field_t *field,
                                           bb_json_t value_item,
                                           char *out, size_t out_cap);

// A displayable field: describes ONE value already present in bb_cache.
// `attrs` makes the field directly bb_filter-selectable (embed-and-recover
// via bb_attrs_container_of, standard intrusive-membership idiom).
//
//   cache_key  — the bb_cache key holding the {ts_ms,data} envelope.
//   json_path  — top-level field name to read out of the envelope's "data"
//                object (v1: flat single-level lookup only).
//   label      — short human label for the default "label: value unit" row.
//   unit       — optional unit suffix (may be NULL/"").
//   kind       — value's scalar kind (drives the default formatter).
//   format     — NULL = bb_sink_display_format_default(); non-NULL overrides.
struct bb_sink_display_field_s {
    bb_attrs_t                 attrs;
    const char                 *cache_key;
    const char                 *json_path;
    const char                 *label;
    const char                 *unit;
    bb_sink_display_kind_t     kind;
    bb_sink_display_format_fn  format;
};

// ---------------------------------------------------------------------------
// Board capabilities -- the WHAT axis (caps x priority -> subset)
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t  screen_tier;      // maps to bb_filter_selector_t.priority_max
    uint16_t max_fields;       // maps to bb_filter_selector_t.max_count
    bool     supports_lvgl;
    uint16_t supported_kinds;  // bitmask: bit (1u << bb_sink_display_kind_t)
} bb_sink_display_caps_t;

// ---------------------------------------------------------------------------
// Render policy -- the HOW axis (driver+policy -> treatment)
// ---------------------------------------------------------------------------

typedef enum {
    BB_SINK_DISPLAY_POLICY_DEFAULT_LINES = 0,  // bb_display_draw_text per row
    BB_SINK_DISPLAY_POLICY_CUSTOM,             // hand rows to cfg->custom
} bb_sink_display_policy_t;

// One row = one selected field's current rendered state.
typedef struct {
    const bb_sink_display_field_t *field;
    char                            value[BB_SINK_DISPLAY_VALUE_MAX];
    bool                            stale;  // age >= stale_after_ms
} bb_sink_display_row_t;

// CUSTOM policy render callback: `rows[0..n_rows)` is the selected +
// formatted subset in selection order. The app owns rendering (e.g. an
// lv_subject bridge) -- this component never calls bb_display in CUSTOM
// mode.
typedef void (*bb_sink_display_render_fn)(const bb_sink_display_row_t *rows,
                                           size_t n_rows, void *ctx);

typedef struct {
    bb_sink_display_policy_t   kind;
    bb_sink_display_render_fn  custom;      // required iff kind == CUSTOM
    void                       *ctx;        // passed through to custom
    uint32_t                   rate_limit_ms;  // coalesced redraw tick period
    uint32_t                   stale_after_ms; // age -> row->stale = true
    uint32_t                   evict_after_ms; // age -> row dropped

    // Reserved multi-display seam (NOT built in v1). NULL (default) selects
    // the bb_display singleton (DEFAULT_LINES calls its free functions
    // directly, link-time backend selection). A non-NULL value is reserved
    // for a future bb_display multi-instance refactor and is rejected by
    // bb_sink_display_init() with BB_ERR_UNSUPPORTED today -- do not set it.
    const void                 *display;
} bb_sink_display_config_t;

// ---------------------------------------------------------------------------
// Pure building blocks (platform/host/bb_sink_display/*.c) -- no bb_cache,
// bb_cache_reactive, bb_timer, or bb_display calls anywhere below. Fully
// host-testable in isolation; the espidf glue
// (platform/espidf/bb_sink_display/bb_sink_display.c) is the ONLY caller on
// device -- one code path, no mirror.
// ---------------------------------------------------------------------------

// caps -> bb_filter_selector_t adapter (pure): max_fields -> max_count,
// supported_kinds -> kind_mask, screen_tier -> priority_max, tag_mask = 0
// (unused in v1), pressure = 0 (no live pressure source wired -- see
// bb_filter's own deferred-pressure-seam doc), min_delivery = MUST (admit
// every field; v1 fields carry no delivery_class distinction).
void bb_sink_display_caps_to_selector(const bb_sink_display_caps_t *caps,
                                       bb_filter_selector_t *out);

// Select the subset of `fields[0..n_fields)` matching `caps`, writing up to
// `out_cap` field pointers into `out` in bb_filter_select()'s priority order.
// Builds a {attrs,item} view over `fields` internally (bounded to
// BB_SINK_DISPLAY_MAX_FIELDS candidates) and recovers each selected field via
// bb_attrs_container_of(). Returns the number of fields written.
size_t bb_sink_display_select(const bb_sink_display_field_t *fields, size_t n_fields,
                               const bb_sink_display_caps_t *caps,
                               const bb_sink_display_field_t **out, size_t out_cap);

// Default formatter: "label: value unit" (unit segment omitted when
// field->unit is NULL/""). Dispatches on field->kind:
// INT/FLOAT/BOOL/STRING via the matching bb_json_item_get_*/is_true accessor;
// an item that doesn't match the declared kind renders as "label: --".
void bb_sink_display_format_default(const bb_sink_display_field_t *field,
                                     bb_json_t value_item,
                                     char *out, size_t out_cap);

// Resolve `field`'s current value out of a bb_cache "data" object's raw JSON
// bytes (i.e. the envelope's "data" segment -- NOT the {ts_ms,data} envelope
// itself; bb_cache_reactive's on_change already hands this shape). Parses
// `data_json[0..data_len)`, looks up field->json_path, and formats via
// field->format (or bb_sink_display_format_default() when NULL) into
// `out`/`out_cap`. Returns false (out left untouched) if data_json fails to
// parse or json_path is absent.
bool bb_sink_display_resolve_field(const bb_sink_display_field_t *field,
                                    const char *data_json, size_t data_len,
                                    char *out, size_t out_cap);

// ---------------------------------------------------------------------------
// Row table -- pure bookkeeping over a fixed-capacity row array. Owns no
// clock, cache, or display access; the glue layer supplies `now_ms` and
// resolved value strings from bb_sink_display_resolve_field().
// ---------------------------------------------------------------------------

typedef struct {
    bb_sink_display_row_t row;       // row.field == NULL means the slot is empty
    uint64_t                last_seen_ms;  // ts_ms of the last applied change
    bool                    dirty;          // needs redraw (minimal-redraw seam)
} bb_sink_display_table_entry_t;

typedef struct {
    bb_sink_display_table_entry_t entries[BB_SINK_DISPLAY_MAX_FIELDS];
    size_t                          n_entries;
} bb_sink_display_table_t;

// Zero the table (all slots empty).
void bb_sink_display_table_init(bb_sink_display_table_t *t);

// Add a row for `field` if not already present (idempotent). No-op (returns
// BB_OK) if `field` already has a row. Returns BB_ERR_NO_SPACE if the table
// is full.
bb_err_t bb_sink_display_table_add(bb_sink_display_table_t *t,
                                    const bb_sink_display_field_t *field);

// Remove every row whose field->cache_key matches `cache_key` (a cache key
// may back more than one selected field/json_path). Returns the number of
// rows removed.
size_t bb_sink_display_table_remove_by_key(bb_sink_display_table_t *t,
                                            const char *cache_key);

// Apply a resolved value to `field`'s own row (matched by field pointer
// identity, NOT cache_key -- a single cache_key on_change may back several
// selected fields with different json_paths/values, so the glue layer
// resolves each field individually via bb_sink_display_resolve_field() and
// applies the result per-field here). Copies `value` into row.value, stamps
// last_seen_ms = ts_ms, clears row.stale, and marks the row dirty. Returns
// BB_ERR_NOT_FOUND if `field` has no row (e.g. not selected).
bb_err_t bb_sink_display_table_apply_change(bb_sink_display_table_t *t,
                                             const bb_sink_display_field_t *field,
                                             const char *value, uint64_t ts_ms);

// Two-stage ts_ms freshness sweep (piggybacks the rate_limit_ms redraw
// tick -- pure push, no polling of its own). For every row:
//   age = now_ms - last_seen_ms
//   age >= evict_after_ms  -> row dropped (slot freed), counts toward the
//                             return value's high 16 bits... no: see below.
//   age >= stale_after_ms  -> row.stale = true, row marked dirty on the
//                             FRESH->STALE transition only (no repeat churn).
// Uses bb_cache_evaluate_age() (bb_cache.h) as the shared classifier -- no
// hand-rolled staleness math here.
// Returns the number of rows dropped by this sweep.
size_t bb_sink_display_table_sweep(bb_sink_display_table_t *t, uint64_t now_ms,
                                    uint32_t stale_after_ms, uint32_t evict_after_ms);

// Collect up to `out_cap` currently-dirty rows into `out` (minimal-redraw:
// only rows that changed since the last collect), clearing their dirty
// flags. Returns the number of rows written.
size_t bb_sink_display_table_collect_dirty(bb_sink_display_table_t *t,
                                            const bb_sink_display_row_t **out,
                                            size_t out_cap);

// Validate `caps`/`cfg` against every caps-independent invariant enforced by
// bb_sink_display_init() -- NULL checks, the CUSTOM-policy cfg->custom
// requirement, the reserved multi-display seam gate (cfg->display), and
// evict_after_ms > stale_after_ms (a value <= stale_after_ms would evict a
// row at or before the age it goes stale, so the STALE state -- and the
// grey-out rendering it drives -- would never be observable). Pure: no
// bb_cache, bb_cache_reactive, bb_timer, or bb_display calls. Called by
// bb_sink_display_init() before touching any module state; host-testable in
// isolation.
//
// Returns BB_ERR_INVALID_ARG if caps or cfg is NULL, if cfg->kind ==
// BB_SINK_DISPLAY_POLICY_CUSTOM and cfg->custom is NULL, or if
// evict_after_ms <= stale_after_ms.
// Returns BB_ERR_UNSUPPORTED if cfg->display is non-NULL (reserved seam,
// see bb_sink_display_config_t.display).
bb_err_t bb_sink_display_validate_config(const bb_sink_display_caps_t *caps,
                                          const bb_sink_display_config_t *cfg);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Select the board's field subset (caps x priority) from the v1 static field
// catalog and store cfg for bb_sink_display_start(). Does not touch
// bb_cache_reactive, bb_timer, or bb_display -- those are wired by
// bb_sink_display_start().
//
// Validates caps/cfg via bb_sink_display_validate_config() first -- see that
// function's doc comment for the exact rejection conditions.
bb_err_t bb_sink_display_init(const bb_sink_display_caps_t *caps,
                               const bb_sink_display_config_t *cfg);

// Register one bb_cache_reactive observer per distinct selected cache_key
// and the shared coalesced redraw bb_timer tick (period = cfg->rate_limit_ms).
// Call from PRE_HTTP tier, after bb_sink_display_init().
bb_err_t bb_sink_display_start(void);

#ifdef __cplusplus
}
#endif
