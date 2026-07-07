#include "bb_meminfo.h"

#include <stdio.h>
#include <string.h>

// Host has no heap_caps_* equivalent — every field zeroed, mirroring
// bb_board_host.c's existing host-stub convention.
bb_err_t bb_meminfo_get(bb_meminfo_snapshot_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    return BB_OK;
}

// Pure formatting — identical on host and ESP-IDF (duplicated in each
// platform impl per bb_meminfo's own convention; see bb_meminfo.h).
int bb_meminfo_format(const bb_meminfo_snapshot_t *snap, char *buf, size_t len)
{
    if (!snap || !buf || len == 0) return 0;
    return snprintf(buf, len,
                     "heap_int_free=%u int_min=%u int_largest=%u "
                     "spiram_free=%u dma_free=%u esp_min_free=%u",
                     (unsigned)snap->internal.free,
                     (unsigned)snap->internal.min_ever_free,
                     (unsigned)snap->internal.largest_free_block,
                     (unsigned)snap->spiram.free,
                     (unsigned)snap->dma.free,
                     (unsigned)snap->esp_min_free_heap);
}
