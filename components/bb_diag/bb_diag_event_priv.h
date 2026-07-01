#pragma once
// Private shared header: canonical snapshot + serializer for the diag.boot
// retained cache topic. No ESP-IDF or FreeRTOS types here. Included by:
//   - platform/espidf/bb_diag/bb_diag_routes.c
//   - components/bb_diag/bb_diag_event_common.c
//   - test/test_host/test_bb_diag_event.c
//   - test/test_host/test_bb_cache_fidelity.c

#include "bb_cache.h"
#include "bb_json.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define BB_DIAG_BOOT_TOPIC "diag.boot"

// Owned snapshot struct for the diag.boot bb_cache entry.
typedef struct {
    char     reset_reason[16];  // e.g. "power-on", "panic", "task_wdt"
    uint32_t wdt_resets;
    bool     panic_available;
    uint32_t panic_boots_since; // only meaningful when panic_available=true
    bool     pending_verify;
    bool     rolled_back;
} bb_diag_boot_snap_t;

// Serializer — signature matches bb_cache_serialize_fn.
// Emits the canonical NESTED shape (same as REST GET /api/diag/boot).
void bb_diag_boot_serialize(bb_json_t obj, const void *snap);
