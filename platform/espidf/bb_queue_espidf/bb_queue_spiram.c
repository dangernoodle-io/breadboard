// ESP-IDF SPIRAM allocator override for bb_queue.
//
// bb_queue buffers (capacity × max_entry_bytes) can be large; routing them to
// SPIRAM frees internal heap for TLS, stack, and real-time paths.
// Fallback to MALLOC_CAP_DEFAULT preserves behaviour on boards without PSRAM.
//
// Registered at EARLY tier so the override is in place before any
// bb_queue_create() call during component init.

#include "bb_queue_espidf.h"
#include "bb_queue.h"
#include "bb_mem.h"

// Called via the bb_app_init() composition root (bbtool:init marker in
// bb_queue_espidf.h), not self-registered.
bb_err_t bb_queue_spiram_early_init(void)
{
    bb_queue_set_allocator(bb_calloc_prefer_spiram, bb_mem_free);
    return BB_OK;
}
