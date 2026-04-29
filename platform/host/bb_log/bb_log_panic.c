#include "bb_log.h"
#include "bb_core.h"

// Host stubs for panic capture (always disabled on host)

bool bb_log_panic_available(void)
{
    return false;
}

bb_err_t bb_log_panic_get(char *out, size_t *len_inout)
{
    (void)out;
    (void)len_inout;
    return BB_ERR_NOT_FOUND;
}

void bb_log_panic_clear(void)
{
}
