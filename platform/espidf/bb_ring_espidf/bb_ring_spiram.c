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
#include "bb_mem.h"
#include "bb_registry.h"

static bb_err_t bb_ring_spiram_early_init(void)
{
    bb_ring_set_allocator(bb_calloc_prefer_spiram, bb_mem_free);
    return BB_OK;
}

BB_REGISTRY_REGISTER_EARLY(bb_ring_spiram, bb_ring_spiram_early_init);
