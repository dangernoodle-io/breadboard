// ESP-IDF SPIRAM allocator override for bb_ring.
//
// bb_ring buffers (capacity × max_entry_bytes) can be large; routing them to
// SPIRAM frees internal heap for TLS, stack, and real-time paths.
// Fallback to MALLOC_CAP_DEFAULT preserves behaviour on boards without PSRAM.
//
// Registered at EARLY tier so the override is in place before any
// bb_ring_create() call during component init.

#include "bb_ring.h"
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

static bb_err_t bb_ring_spiram_early_init(void)
{
    bb_ring_set_allocator(spiram_calloc, spiram_free);
    return BB_OK;
}

BB_REGISTRY_REGISTER_EARLY(bb_ring_spiram, bb_ring_spiram_early_init);
