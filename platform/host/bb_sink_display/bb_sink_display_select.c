// bb_sink_display caps->selector adapter + bb_filter-backed field selection.
// Pure: no bb_cache/bb_cache_reactive/bb_timer/bb_display calls. Shared
// verbatim between host tests and the espidf glue
// (platform/espidf/bb_sink_display/bb_sink_display.c) -- one code path.

#include "bb_sink_display.h"

void bb_sink_display_caps_to_selector(const bb_sink_display_caps_t *caps,
                                       bb_filter_selector_t *out)
{
    if (!out) return;
    if (!caps) {
        *out = (bb_filter_selector_t){0};
        return;
    }
    out->max_count    = caps->max_fields;
    out->priority_max  = caps->screen_tier;
    out->kind_mask     = caps->supported_kinds;
    out->tag_mask      = 0;
    out->pressure      = 0;
    out->min_delivery  = BB_ATTRS_DELIVERY_MUST;
}

size_t bb_sink_display_select(const bb_sink_display_field_t *fields, size_t n_fields,
                               const bb_sink_display_caps_t *caps,
                               const bb_sink_display_field_t **out, size_t out_cap)
{
    if (!fields || !caps || !out || out_cap == 0) return 0;
    if (n_fields > BB_SINK_DISPLAY_MAX_FIELDS) n_fields = BB_SINK_DISPLAY_MAX_FIELDS;

    bb_filter_elem_t elems[BB_SINK_DISPLAY_MAX_FIELDS];
    for (size_t i = 0; i < n_fields; i++) {
        elems[i].attrs = &fields[i].attrs;
        elems[i].item  = &fields[i];
    }

    bb_filter_selector_t sel;
    bb_sink_display_caps_to_selector(caps, &sel);

    bb_filter_elem_t selected[BB_SINK_DISPLAY_MAX_FIELDS];
    size_t cap = out_cap < BB_SINK_DISPLAY_MAX_FIELDS ? out_cap : BB_SINK_DISPLAY_MAX_FIELDS;
    size_t n = bb_filter_select(elems, n_fields, &sel, selected, cap);

    for (size_t i = 0; i < n; i++) {
        out[i] = bb_attrs_container_of(selected[i].attrs, bb_sink_display_field_t, attrs);
    }
    return n;
}
