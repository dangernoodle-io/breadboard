// bb_sink_display default formatter + value resolver. Pure: no
// bb_cache/bb_cache_reactive/bb_timer/bb_display calls -- only bb_json,
// which is portable (host + espidf backends behave identically here). One
// code path, shared verbatim by host tests and the espidf glue.

#include "bb_sink_display.h"

#include <stdio.h>

void bb_sink_display_format_default(const bb_sink_display_field_t *field,
                                     bb_json_t value_item,
                                     char *out, size_t out_cap)
{
    if (!field || !out || out_cap == 0) return;

    const char *label = field->label ? field->label : "";
    const char *unit  = field->unit  ? field->unit  : "";
    char valbuf[24];
    bool ok = false;

    switch (field->kind) {
    case BB_SINK_DISPLAY_KIND_INT:
        if (value_item && bb_json_item_is_number(value_item)) {
            snprintf(valbuf, sizeof(valbuf), "%d", bb_json_item_get_int(value_item));
            ok = true;
        }
        break;
    case BB_SINK_DISPLAY_KIND_FLOAT:
        if (value_item && bb_json_item_is_number(value_item)) {
            snprintf(valbuf, sizeof(valbuf), "%.2f", bb_json_item_get_double(value_item));
            ok = true;
        }
        break;
    case BB_SINK_DISPLAY_KIND_BOOL:
        if (value_item) {
            snprintf(valbuf, sizeof(valbuf), "%s", bb_json_item_is_true(value_item) ? "true" : "false");
            ok = true;
        }
        break;
    case BB_SINK_DISPLAY_KIND_STRING:
        if (value_item && bb_json_item_is_string(value_item)) {
            // bb_json_item_is_string() already confirmed value_item is a
            // string node -- bb_json_item_get_string() is guaranteed
            // non-NULL for that node type, so no NULL fallback is needed.
            snprintf(valbuf, sizeof(valbuf), "%s", bb_json_item_get_string(value_item));
            ok = true;
        }
        break;
    default:
        // Defensive residual: field->kind is caller-supplied data (a static
        // const array entry), not an invariant the compiler enforces --
        // hit directly by test_bb_sink_display_format_default_unknown_kind.
        break;
    }

    if (!ok) {
        snprintf(out, out_cap, "%s: --", label);
        return;
    }
    if (unit[0] != '\0') {
        snprintf(out, out_cap, "%s: %s %s", label, valbuf, unit);
    } else {
        snprintf(out, out_cap, "%s: %s", label, valbuf);
    }
}

bool bb_sink_display_resolve_field(const bb_sink_display_field_t *field,
                                    const char *data_json, size_t data_len,
                                    char *out, size_t out_cap)
{
    if (!field || !data_json || data_len == 0 || !out || out_cap == 0) return false;

    bb_json_t doc = bb_json_parse(data_json, data_len);
    if (!doc) return false;

    bb_json_t item = bb_json_obj_get_item(doc, field->json_path);
    if (!item) {
        bb_json_free(doc);
        return false;
    }

    bb_sink_display_format_fn fn = field->format ? field->format : bb_sink_display_format_default;
    fn(field, item, out, out_cap);

    bb_json_free(doc);
    return true;
}
