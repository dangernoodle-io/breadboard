#include "bb_meminfo.h"

#include <string.h>

// Host has no heap_caps_* equivalent — every field zeroed, mirroring
// bb_board_host.c's existing host-stub convention.
bb_err_t bb_meminfo_get(bb_meminfo_snapshot_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    return BB_OK;
}
