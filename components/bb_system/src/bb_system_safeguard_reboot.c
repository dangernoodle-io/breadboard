// bb_system WiFi safeguard-reboot facade (B1-790 slice) -- collapses the
// wifi_reconn FSM's reboot adapter from 5 hooks to 2 and closes a
// validated-but-unsynced throttle gap (B1-1002, B1-1003, B1-884). See
// bb_system.h's facade block for the full rationale.
//
// Portable: compiled on host AND ESP-IDF, no ESP_PLATFORM gate -- every
// function this file calls (bb_system_boot_count_get/increment,
// bb_system_boot_fail_count_over, bb_system_reboot_budget_allows_at/record)
// is already dispatched correctly per-platform, so the orchestration itself
// needs no split. Only the epoch/synced-resolving public wrappers
// (bb_system_safeguard_reboot_allowed/bb_system_safeguard_reboot) are
// per-platform, mirroring bb_system_reboot_budget_allows/record.
#include "bb_system.h"
#include "bb_log.h"

static const char *TAG = "bb_system_safeguard_reboot";

bool bb_system_safeguard_reboot_should_increment(bool ota_validated, bool synced)
{
    return !(synced && ota_validated);
}

bb_reset_source_t bb_system_safeguard_reboot_src_for_cause(bb_reboot_cause_t cause)
{
    switch (cause) {
    case BB_REBOOT_CAUSE_WIFI_SAFEGUARD: return BB_RESET_SRC_WIFI_SAFEGUARD;
    case BB_REBOOT_CAUSE_EGRESS_TIER3:   return BB_RESET_SRC_EGRESS_TIER3;
    default:                             return BB_RESET_SRC_UNKNOWN;
    }
}

bool bb_system_safeguard_reboot_allowed_at(bb_reboot_cause_t cause, bool synced, uint32_t now_s)
{
    if (synced) {
        return bb_system_reboot_budget_allows_at(cause, /*synced=*/true, now_s);
    }
    return !bb_system_boot_fail_count_over(bb_system_boot_count_get(), BB_SYSTEM_BOOT_FAIL_THRESHOLD);
}

void bb_system_safeguard_reboot_account(bb_reboot_cause_t cause, bool ota_validated, bool synced)
{
    if (bb_system_safeguard_reboot_should_increment(ota_validated, synced)) {
        (void)bb_system_boot_count_increment();
    }
    bb_system_reboot_budget_record(cause);
    bb_log_w(TAG, "safeguard reboot: cause=%u fail_count=%u threshold=%u",
              (unsigned)cause, (unsigned)bb_system_boot_count_get(), (unsigned)BB_SYSTEM_BOOT_FAIL_THRESHOLD);
}
