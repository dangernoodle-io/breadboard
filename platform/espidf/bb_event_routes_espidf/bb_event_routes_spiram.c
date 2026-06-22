// ESP-IDF SPIRAM allocator override for bb_event_routes.
//
// bb_event_routes per-client queue buffers (default 8×256 = 2 KB per client)
// are sized to live in SPIRAM on boards with PSRAM, keeping the ~2 KB per
// client out of the ~30 KB internal-heap budget shared with TLS handshakes.
// Fallback to MALLOC_CAP_DEFAULT preserves behaviour on boards without PSRAM.
//
// Called from bb_event_routes_register_routes_init before bb_event_routes_init
// so the override is in place before any client slot is allocated.
#include "bb_event_routes_internal.h"
#include "bb_mem.h"

void bb_event_routes_spiram_init(void)
{
    bb_event_routes_set_allocator(bb_calloc_prefer_spiram, bb_mem_free);
}
