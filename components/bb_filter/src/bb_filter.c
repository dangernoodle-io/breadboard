// bb_filter — pure projection over elements carrying bb_attrs.
// See include/bb_filter.h for the full contract. No allocation, no state,
// no lock, no task, no clock read: every function here is a deterministic
// transform of its arguments only.
#include "bb_filter.h"

#include <stdbool.h>

// True when `a` passes every gate in `sel` (priority_max / kind_mask /
// tag_mask / min_delivery).
static bool bb_filter_gate_pass(const bb_attrs_t *a, const bb_filter_selector_t *sel)
{
    if (a->priority > sel->priority_max) {
        return false;
    }
    if (sel->kind_mask != 0) {
        // kind_mask is 16 bits wide; kind >= 16 can never match a non-zero
        // mask, and shifting by >= 16 on a 16-bit-domain mask would also
        // never set a bit we'd test — reject up front rather than compute
        // a shift whose result we already know cannot match.
        if (a->kind >= 16) {
            return false;
        }
        if (((uint32_t)1u << a->kind & sel->kind_mask) == 0) {
            return false;
        }
    }
    if (sel->tag_mask != 0 && (a->tag_mask & sel->tag_mask) == 0) {
        return false;
    }
    if (a->delivery_class < sel->min_delivery) {
        return false;
    }
    return true;
}

// Find the index of the next candidate in (priority, index) order strictly
// after (have_last ? (last_priority, last_index) : -infinity). Returns n
// when no further candidate exists. This performs a stable selection-sort
// step without any auxiliary "already selected" storage.
static size_t bb_filter_find_next(const bb_filter_elem_t     *in,
                                   size_t                      n,
                                   const bb_filter_selector_t *sel,
                                   bool                        have_last,
                                   uint8_t                     last_priority,
                                   size_t                      last_index)
{
    size_t best = n;

    for (size_t i = 0; i < n; i++) {
        const bb_attrs_t *a = in[i].attrs;
        if (a == NULL || !bb_filter_gate_pass(a, sel)) {
            continue;
        }

        bool after_last;
        if (!have_last) {
            after_last = true;
        } else if (a->priority > last_priority) {
            after_last = true;
        } else if (a->priority == last_priority && i > last_index) {
            after_last = true;
        } else {
            after_last = false;
        }
        if (!after_last) {
            continue;
        }

        if (best == n) {
            best = i;
            continue;
        }
        // Ties never replace `best`: the scan runs i ascending, so any
        // equal-priority candidate found later than `best` already has a
        // larger index and must lose the stability tie-break — only a
        // strictly smaller priority can ever displace the current best.
        const bb_attrs_t *ba = in[best].attrs;
        if (a->priority < ba->priority) {
            best = i;
        }
    }

    return best;
}

size_t bb_filter_select(const bb_filter_elem_t     *in,
                         size_t                      n,
                         const bb_filter_selector_t *sel,
                         bb_filter_elem_t           *out,
                         size_t                      out_cap)
{
    if (sel == NULL || out == NULL || out_cap == 0 || in == NULL || n == 0) {
        return 0;
    }

    // Pass 1: count gated elements overall and gated DEFERRABLE elements,
    // to derive the pressure-shed count without any extra storage.
    size_t deferrable_total = 0;
    for (size_t i = 0; i < n; i++) {
        const bb_attrs_t *a = in[i].attrs;
        if (a == NULL || !bb_filter_gate_pass(a, sel)) {
            continue;
        }
        if (a->delivery_class == BB_ATTRS_DELIVERY_DEFERRABLE) {
            deferrable_total++;
        }
    }

    size_t shed_count = 0;
    if (sel->pressure > 0 && deferrable_total > 0) {
        shed_count = ((size_t)deferrable_total * sel->pressure) / 255u;
    }

    size_t target_count = (sel->max_count == 0) ? n : (size_t)sel->max_count;

    // Pass 2: stable-select in ascending-priority order, shedding the
    // tail-most `shed_count` DEFERRABLE elements as we go.
    bool     have_last     = false;
    uint8_t  last_priority = 0;
    size_t   last_index    = 0;
    size_t   deferrable_seen = 0;
    size_t   written        = 0;

    while (written < target_count && written < out_cap) {
        size_t idx = bb_filter_find_next(in, n, sel, have_last, last_priority, last_index);
        if (idx == n) {
            break;
        }

        const bb_attrs_t *a = in[idx].attrs;
        have_last     = true;
        last_priority = a->priority;
        last_index    = idx;

        if (a->delivery_class == BB_ATTRS_DELIVERY_DEFERRABLE) {
            deferrable_seen++;
            size_t remaining_after_this = deferrable_total - deferrable_seen;
            if (remaining_after_this < shed_count) {
                continue;
            }
        }

        out[written++] = in[idx];
    }

    return written;
}

bb_filter_emit_t bb_filter_emit_decide(const bb_attrs_t           *attrs,
                                        const bb_filter_selector_t *sel,
                                        uint32_t                    since_last_ms)
{
    if (attrs == NULL || sel == NULL) {
        return BB_FILTER_EMIT_NOW;
    }
    if (sel->pressure == 0) {
        return BB_FILTER_EMIT_NOW;
    }

    if (attrs->delivery_class == BB_ATTRS_DELIVERY_MUST) {
        return (since_last_ms == 0) ? BB_FILTER_EMIT_DEFER : BB_FILTER_EMIT_NOW;
    }

    uint32_t floor_ms = (uint32_t)sel->pressure * BB_FILTER_PRESSURE_MS_PER_UNIT;
    if (since_last_ms >= floor_ms * 2u) {
        return BB_FILTER_EMIT_DROP;
    }
    if (since_last_ms >= floor_ms) {
        return BB_FILTER_EMIT_NOW;
    }
    return BB_FILTER_EMIT_DEFER;
}
