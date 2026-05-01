#include "bb_diag.h"
#include "bb_core.h"

// Host stubs for panic capture (always disabled on host)

bool bb_diag_panic_available(void)
{
    return false;
}

bb_err_t bb_diag_panic_get(char *out, size_t *len_inout)
{
    (void)out;
    (void)len_inout;
    return BB_ERR_NOT_FOUND;
}

void bb_diag_panic_clear(void)
{
}

bool bb_diag_panic_coredump_available(void)
{
    return false;
}

bb_err_t bb_diag_panic_coredump_get(bb_diag_panic_summary_t *out)
{
    (void)out;
    return BB_ERR_NOT_FOUND;
}

uint32_t bb_diag_panic_boots_since(void)
{
    return 0;
}
