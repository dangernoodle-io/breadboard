// bb_diag_meminfo -- see bb_diag_meminfo.h for the section contract. Thin
// adapter: bb_diag_fill_fn's signature is untyped (void *dst, const
// bb_diag_fill_args_t *args), so this shim is the only cast needed over
// bb_meminfo_heap_snap_fill()'s typed signature.

#include "bb_diag_meminfo.h"

#include "bb_meminfo_heap_snap.h"

bb_err_t bb_diag_meminfo_fill(void *dst, const bb_diag_fill_args_t *args)
{
    (void)args;
    return bb_meminfo_heap_snap_fill((bb_meminfo_heap_snap_t *)dst);
}

#ifdef ESP_PLATFORM
bb_err_t bb_diag_meminfo_register(void)
{
    bb_diag_section_t section = {
        .name         = "meminfo",
        .desc         = "heap memory snapshot",
        .snap_desc    = &bb_meminfo_heap_snap_desc,
        .fill         = bb_diag_meminfo_fill,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    return bb_diag_register_section(&section);
}
#endif
