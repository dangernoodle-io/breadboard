// Registers the JSON backend into bb_serialize's format-dispatch registry
// (bb_serialize_format_register()). See bb_serialize_json_register_format()'s
// doc comment in bb_serialize_json.h for the render/parse contract.
#include "bb_serialize_json.h"

#include "bb_serialize_format.h"

static const bb_serialize_format_entry_t s_json_format_entry = {
    .render = bb_serialize_json_render,
    .parse  = bb_serialize_json_parse_bytes,
};

bb_err_t bb_serialize_json_register_format(void)
{
    return bb_serialize_format_register(BB_FORMAT_JSON, &s_json_format_entry);
}
