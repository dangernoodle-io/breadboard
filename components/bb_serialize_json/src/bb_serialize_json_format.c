// Registers the JSON backend into bb_serialize's format-dispatch registry
// (bb_serialize_format_register()). See bb_serialize_json_register_format()'s
// doc comment in bb_serialize_json.h for the render/parse contract.
#include "bb_serialize_json.h"

#include "bb_serialize_format.h"

// Adapter matching bb_serialize_render_fn's fixed, format-agnostic shape
// (shared across every bb_serialize_format backend) -- bb_serialize_json_render()
// itself now takes a bb_serialize_json_render_cfg_t (B1-1437) and so no
// longer matches that typedef directly. Always renders with the interface's
// implicit default (f64_shortest == false); a caller that needs
// shortest-round-trippable f64 output calls bb_serialize_json_render()
// directly, bypassing the format-dispatch registry.
static bb_err_t bb_json_format_render(const bb_serialize_desc_t *desc, const void *snap,
                                       char *buf, size_t cap, size_t *out_len)
{
    bb_serialize_json_render_cfg_t cfg = {
        .desc = desc,
        .snap = snap,
        .buf  = buf,
        .cap  = cap,
    };
    return bb_serialize_json_render(&cfg, out_len);
}

static const bb_serialize_format_entry_t s_json_format_entry = {
    .render = bb_json_format_render,
    .parse  = bb_serialize_json_parse_bytes,
};

bb_err_t bb_serialize_json_register_format(void)
{
    return bb_serialize_format_register(BB_FORMAT_JSON, &s_json_format_entry);
}
