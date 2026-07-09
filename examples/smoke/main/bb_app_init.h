#pragma once
// Hand-authored prototypes for smoke's `bbtool codegen`-generated
// composition root (generated/bb_app_init.c, gitignored -- decision #725).
// codegen emits no header of its own (wire.py's `render_source` only
// emits the .c + a link-set .cmake fragment), so the entry point that
// calls these functions declares them itself; signatures are fixed by
// commands/wire.py's `render_source` (see that module's docstring):
// three zero-arg entry points, all returning bb_err_t, first-error
// semantics per tier.
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

bb_err_t bb_app_init_early(void);
bb_err_t bb_app_init_rest(void);
bb_err_t bb_app_init(void);

#ifdef __cplusplus
}
#endif
