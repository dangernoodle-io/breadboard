// Registers the console backend into bb_serialize's format-dispatch
// registry (bb_serialize_format_register()). See
// bb_serialize_console_register_format()'s doc comment in
// bb_serialize_console.h for the render/parse contract.
#include "bb_serialize_console.h"

#include "bb_serialize_format.h"

static const bb_serialize_format_entry_t s_console_format_entry = {
    .render = bb_serialize_console_render,
    .parse = NULL,  // render-only backend, no ingest side
};

bb_err_t bb_serialize_console_register_format(void)
{
    return bb_serialize_format_register(BB_FORMAT_CONSOLE, &s_console_format_entry);
}
