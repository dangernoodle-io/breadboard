// ESP-IDF SPIRAM allocator override for bb_event_ring.
//
// bb_event_ring per-topic ring buffers (default 16×256 = 4 KB per topic) are
// sized to live in SPIRAM on boards with PSRAM so the ~4 KB per topic does not
// eat into the ~30 KB internal-heap budget shared with TLS handshakes.
// Fallback to MALLOC_CAP_DEFAULT preserves behaviour on boards without PSRAM.
//
// Registered at EARLY tier so the override is in place before any
// bb_event_ring_attach_ex call during component init.
#include "bb_event_ring_internal.h"
#include "bb_core.h"
#include "bb_registry.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

static void *spiram_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_calloc(n, sz, MALLOC_CAP_DEFAULT);
    }
    return p;
}

static void spiram_free(void *p)
{
    heap_caps_free(p);
}

static bb_err_t bb_event_ring_spiram_early_init(void)
{
    bb_event_ring_set_allocator(spiram_calloc, spiram_free);
    return BB_OK;
}

BB_REGISTRY_REGISTER_EARLY(bb_event_ring_spiram, bb_event_ring_spiram_early_init);
