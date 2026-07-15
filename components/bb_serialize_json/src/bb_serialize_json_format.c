// Registers the JSON backend into bb_serialize's format-dispatch registry
// (bb_serialize_format_register()). See bb_serialize_json_register_format()'s
// doc comment in bb_serialize_json.h for the render/parse contract.
#include "bb_serialize_json.h"

#include "bb_serialize_format.h"

static const bb_serialize_format_entry_t s_json_format_entry = {
    .render = bb_serialize_json_render,
    // bb_serialize_json_scan_bounded's signature doesn't match
    // bb_serialize_render_fn -- it's this backend's ingest (read-side) entry
    // point, stored under the registry's deliberately opaque `const void *`
    // parse field. A lookup caller casts it back to
    // `bb_err_t (*)(const char *, size_t, const bb_serialize_json_ingest_t *)`.
    .parse = (const void *)bb_serialize_json_scan_bounded,
};

bb_err_t bb_serialize_json_register_format(void)
{
    return bb_serialize_format_register(BB_FORMAT_JSON, &s_json_format_entry);
}
