#pragma once
// Private shared header: canonical snapshot struct for the diag.boot
// retained cache topic. No ESP-IDF or FreeRTOS types here. Included by:
//   - platform/espidf/bb_diag/bb_diag_routes.c
//   - components/bb_diag/bb_diag_boot_wire.c
//   - test/test_host/test_wire_desc_producers.c
//
// The bb_json bb_cache serializer that used to live here
// (bb_diag_boot_serialize(), components/bb_diag/bb_diag_event_common.c) was
// retired in B1-1053 PR1 -- the REST GET path renders via bb_data
// (bb_diag_boot_gather()/bb_diag_boot_wire_desc, bb_diag_boot_wire.c)
// instead. This key's bb_cache_config_t now registers with
// cfg->serialize == NULL (see bb_cache.h's field doc).

#include "bb_cache.h"
#include "bb_reboot_reason.h"
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

    // Reboot-reason SSOT (B1-527 PR-A). Latched once at boot from the
    // clear-on-read NVS record (see load_reboot_record in bb_diag_routes.c);
    // stays constant across repeated serialize calls within the same boot.
    uint8_t  reboot_src;           // bb_reset_source_t
    char     reboot_detail[49];    // may be empty
    uint32_t reboot_epoch_s;       // 0 = unknown/unsynced at reboot time
    uint32_t reboot_uptime_s;      // prior-session uptime (seconds) at reboot

    // Rolling reboot history ring (B1-527 PR-B). Latched once at boot
    // alongside reboot_src/reboot_detail/etc (see load_reboot_record in
    // bb_diag_routes.c). Unlike the fields above, this ring is NOT
    // cleared-on-read — it accumulates across boots, including untagged /
    // hardware resets (recorded as src=BB_RESET_SRC_UNKNOWN).
    bb_reboot_history_t reboot_history;

    // Current wall clock, refreshed fresh on every publish/serialize call
    // (see build_boot_snap in bb_diag_routes.c) so age_s reflects "now",
    // not the time the cache entry was last updated.
    uint32_t now_epoch_s;
    bool     now_epoch_valid;
} bb_diag_boot_snap_t;
