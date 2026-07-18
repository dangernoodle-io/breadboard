// Registers the logfmt backend into bb_serialize's format-dispatch
// registry (bb_serialize_format_register()). See
// bb_serialize_logfmt_register_format()'s doc comment in
// bb_serialize_logfmt.h for the render/parse contract.
#include "bb_serialize_logfmt.h"

#include "bb_serialize_format.h"

static const bb_serialize_format_entry_t s_logfmt_format_entry = {
    .render = bb_serialize_logfmt_render,
    .parse = NULL,  // render-only backend, no ingest side
};

bb_err_t bb_serialize_logfmt_register_format(void)
{
    return bb_serialize_format_register(BB_FORMAT_LOGFMT, &s_logfmt_format_entry);
}
