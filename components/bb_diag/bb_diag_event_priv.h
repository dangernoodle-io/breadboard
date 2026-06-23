#pragma once
// Private shared header: pure JSON builder for the bb_diag diag.boot retained
// event topic. No ESP-IDF or FreeRTOS types here. Included by:
//   - platform/espidf/bb_diag/bb_diag_routes.c (emit at init)
//   - components/bb_diag/bb_diag_event_common.c (pure impl)
//   - test/test_host/test_bb_diag_event.c (host unit tests)

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define BB_DIAG_BOOT_TOPIC "diag.boot"

// Write JSON payload for a diag.boot event into buf[buf_sz].
// Format: {"reset_reason":"<str>","wdt_resets":<n>,"panic_available":<bool>,"pending_verify":<bool>,"rolled_back":<bool>}
// reset_reason: short string (e.g. "poweron", "panic", "task_wdt"); must not be NULL.
// pending_verify: true when the running partition is in PENDING_VERIFY OTA state (unverified OTA image).
// rolled_back: true when the non-running slot is in ABORTED or INVALID OTA state (bootloader rolled back a previous attempt).
// Returns number of chars that would have been written (like snprintf), -1 on bad args.
int bb_diag_boot_build_json(char *buf, size_t buf_sz,
                             const char *reset_reason,
                             uint32_t abnormal_reset_count,
                             bool panic_available,
                             bool pending_verify,
                             bool rolled_back);
