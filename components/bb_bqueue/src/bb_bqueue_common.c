// bb_bqueue — pure logic shared by the host and ESP-IDF backends: cfg
// bounds validation and the absolute-deadline/remaining-time arithmetic
// bb_lock_cond_wait()'s documentation mandates for any predicate-recheck
// loop (see bb_lock.h). No platform headers here — compiled identically for
// host and ESP-IDF, and directly host-testable independent of real
// blocking. Only the host backend (platform/host/bb_bqueue/bb_bqueue.c)
// actually calls the deadline helpers; the ESP-IDF backend hands its
// timeout straight to FreeRTOS's own tick-based xQueue calls and has no
// predicate-recheck loop of its own.
#include "bb_bqueue_priv.h"

bb_err_t bb_bqueue_validate_cfg(const bb_bqueue_cfg_t *cfg)
{
    if (!cfg) {
        return BB_ERR_INVALID_ARG;
    }
    if (cfg->capacity == 0 || cfg->capacity > BB_BQUEUE_MAX_CAPACITY) {
        return BB_ERR_INVALID_ARG;
    }
    if (cfg->item_bytes == 0 || cfg->item_bytes > BB_BQUEUE_MAX_ITEM_BYTES) {
        return BB_ERR_INVALID_ARG;
    }
    return BB_OK;
}

uint64_t bb_bqueue_deadline_compute(uint64_t now_us, uint32_t timeout_ms)
{
    return now_us + (uint64_t)timeout_ms * 1000u;
}

bool bb_bqueue_deadline_remaining_ms(uint64_t deadline_us, uint64_t now_us, uint32_t *out_remaining_ms)
{
    if (now_us >= deadline_us) {
        return false;
    }
    *out_remaining_ms = (uint32_t)((deadline_us - now_us) / 1000u);
    return true;
}
