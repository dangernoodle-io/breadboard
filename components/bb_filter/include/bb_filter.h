// bb_filter — pure projection over elements carrying bb_attrs.
//
// A collection of elements is never materialized or owned here: the caller
// supplies an array of {attrs, item} pairs, a selector describing which
// subset it wants, and an output buffer. bb_filter never dereferences
// `item` — only `attrs` is read. There is no state, no heap, no lock, no
// task, and no clock read anywhere in this component; every function is a
// pure, deterministic transform of its inputs.
//
// Pressure seam (deliberately unwired): `bb_filter_selector_t.pressure` is a
// bare input parameter. Nothing in breadboard derives it from a live signal
// today — that derivation (some future health/pressure source ->
// derive_pressure() -> sel.pressure) is a DEFERRED, later integration. The
// field exists so a future caller can plug a real pressure source in
// without an API break; do not wire one here.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bb_attrs.h"

#ifdef __cplusplus
extern "C" {
#endif

// One filterable element: attrs is read by bb_filter, item never is.
typedef struct {
    const bb_attrs_t *attrs;
    const void       *item;
} bb_filter_elem_t;

// Selection criteria for bb_filter_select() / bb_filter_emit_decide().
//
//   max_count     0 = unbounded; else truncate the result to at most this
//                 many elements (after gating and pressure shedding).
//   priority_max  Elements with attrs->priority > priority_max are
//                 excluded. 0xFF admits every priority (priority is a
//                 uint8_t, so the comparison is always true).
//   kind_mask     0 = any kind; else an element is included only if bit
//                 (1u << attrs->kind) is set in kind_mask. kind_mask is
//                 16 bits wide, so only kind values 0-15 can ever match a
//                 non-zero mask; kind >= 16 is excluded whenever kind_mask
//                 is non-zero.
//   tag_mask      0 = any; else an element is included only if
//                 (attrs->tag_mask & tag_mask) != 0.
//   pressure      0-255, 0 = no pressure. See the pressure seam note above
//                 — this is a bare input, never derived internally.
//   min_delivery  Elements with attrs->delivery_class < min_delivery are
//                 excluded. BB_ATTRS_DELIVERY_MUST (0) admits every class;
//                 BB_ATTRS_DELIVERY_DEFERRABLE (1) admits only DEFERRABLE
//                 elements.
typedef struct {
    uint16_t max_count;
    uint8_t  priority_max;
    uint16_t kind_mask;
    uint32_t tag_mask;
    uint8_t  pressure;
    uint8_t  min_delivery;
} bb_filter_selector_t;

// bb_filter_emit_decide() outcome. MUST elements (BB_ATTRS_DELIVERY_MUST)
// are never DROPped — only NOW or DEFER. DEFERRABLE elements may reach any
// of the three outcomes.
typedef enum {
    BB_FILTER_EMIT_NOW = 0,
    BB_FILTER_EMIT_DEFER,
    BB_FILTER_EMIT_DROP,
} bb_filter_emit_t;

// Select the subset of `in[0..n)` matching `sel`, writing up to `out_cap`
// results into `out` in ascending-priority order (stable: elements with
// equal priority keep their relative input order).
//
// Pipeline (in this order):
//   1. gate each element by priority_max / kind_mask / tag_mask /
//      min_delivery
//   2. stable-sort the gated set by priority ascending
//   3. pressure-shed: when sel->pressure > 0, shed the worst-priority
//      (tail-most) DEFERRABLE elements first; MUST elements are never
//      shed by pressure. The number shed is
//      floor(deferrable_count * pressure / 255).
//   4. truncate to sel->max_count (0 = unbounded)
//
// The result is additionally capped by out_cap regardless of max_count.
// Returns the number of elements written to `out` (may be less than the
// filtered count when out_cap is smaller).
//
// A NULL `in`/`sel`/`out`, a zero `n`, or a zero `out_cap` yields 0 with no
// writes. Pure and deterministic: no allocation, no global state.
size_t bb_filter_select(const bb_filter_elem_t     *in,
                         size_t                      n,
                         const bb_filter_selector_t *sel,
                         bb_filter_elem_t           *out,
                         size_t                      out_cap);

// Decide whether a single element should emit NOW, be DEFERred (delayed,
// not dropped), or DROPped, given `since_last_ms` elapsed since the last
// emission of this element and the current `sel->pressure`.
//
//   - attrs == NULL || sel == NULL: fail-open, returns BB_FILTER_EMIT_NOW.
//   - sel->pressure == 0: always BB_FILTER_EMIT_NOW.
//   - BB_ATTRS_DELIVERY_MUST: BB_FILTER_EMIT_DEFER when since_last_ms == 0
//     (avoids a redundant back-to-back emit), else BB_FILTER_EMIT_NOW.
//     Never DROP.
//   - BB_ATTRS_DELIVERY_DEFERRABLE: pressure scales an implicit cadence
//     floor (pressure * BB_FILTER_PRESSURE_MS_PER_UNIT). Below the floor:
//     DEFER. At or above the floor but under 2x the floor: NOW. At or
//     above 2x the floor (pressure has been sustained long enough that
//     deferral would grow unbounded): DROP.
//
// Pure and deterministic: no clock read, no state — the caller supplies
// since_last_ms.
#ifndef BB_FILTER_PRESSURE_MS_PER_UNIT
#define BB_FILTER_PRESSURE_MS_PER_UNIT 100u
#endif

bb_filter_emit_t bb_filter_emit_decide(const bb_attrs_t           *attrs,
                                        const bb_filter_selector_t *sel,
                                        uint32_t                    since_last_ms);

#ifdef __cplusplus
}
#endif
