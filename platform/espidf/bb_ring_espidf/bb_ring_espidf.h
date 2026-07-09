// bb_ring_espidf — ESP-IDF SPIRAM allocator override for bb_ring.
//
// Registers bb_ring's allocator to prefer SPIRAM (falling back to
// MALLOC_CAP_DEFAULT on boards without PSRAM), freeing internal heap for
// TLS, stack, and real-time paths. Must run at EARLY tier, before any
// bb_ring_create() call during component init.
#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// bbtool:init tier=early fn=bb_ring_spiram_early_init
bb_err_t bb_ring_spiram_early_init(void);

#ifdef __cplusplus
}
#endif
