// bb_bqueue — private helpers shared by the host and ESP-IDF backends. Not
// part of the public API; included via PRIV_INCLUDE_DIRS "src" from
// platform/{host,espidf}/bb_bqueue/*.c only.
#pragma once

#include "bb_bqueue.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Kconfig bridge (canonical two-step pattern; see bb_tcp_client_priv.h /
// bb_clock.h). On ESP-IDF, Kconfig generates CONFIG_BB_BQUEUE_* symbols.
// Bridge them to the resolved BB_BQUEUE_* macros here so both backends
// (host, espidf) read one already-resolved definition instead of each
// re-deriving its own ad-hoc fallback. Never shadow the generated CONFIG_
// symbol with a bare #ifndef.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_BQUEUE_MAX_INSTANCES
#    define BB_BQUEUE_MAX_INSTANCES CONFIG_BB_BQUEUE_MAX_INSTANCES
#  endif
#  ifdef CONFIG_BB_BQUEUE_MAX_CAPACITY
#    define BB_BQUEUE_MAX_CAPACITY CONFIG_BB_BQUEUE_MAX_CAPACITY
#  endif
#  ifdef CONFIG_BB_BQUEUE_MAX_ITEM_BYTES
#    define BB_BQUEUE_MAX_ITEM_BYTES CONFIG_BB_BQUEUE_MAX_ITEM_BYTES
#  endif
#endif

#ifndef BB_BQUEUE_MAX_INSTANCES
#define BB_BQUEUE_MAX_INSTANCES 2
#endif
#ifndef BB_BQUEUE_MAX_CAPACITY
#define BB_BQUEUE_MAX_CAPACITY 16
#endif
#ifndef BB_BQUEUE_MAX_ITEM_BYTES
#define BB_BQUEUE_MAX_ITEM_BYTES 64
#endif

/**
 * Validate cfg->capacity/item_bytes against the Kconfig maxima above. Pure,
 * host-testable, shared by both backends' bb_bqueue_create() so the bounds
 * check lives in exactly one place (never re-hand-rolled per-backend).
 *
 * @return BB_OK if valid; BB_ERR_INVALID_ARG if cfg is NULL, or
 *         capacity/item_bytes is 0 or exceeds its Kconfig maximum.
 */
bb_err_t bb_bqueue_validate_cfg(const bb_bqueue_cfg_t *cfg);

/**
 * Compute an absolute deadline (microseconds) from now_us + timeout_ms.
 * Pure, host-testable. Part of the MANDATORY absolute-deadline/
 * remaining-time idiom bb_lock_cond_wait()'s own documentation requires of
 * every consumer with a predicate-recheck loop (see bb_lock.h) — factored
 * here rather than re-derived per call site (peek/receive/send all share
 * it).
 */
uint64_t bb_bqueue_deadline_compute(uint64_t now_us, uint32_t timeout_ms);

/**
 * Compute the remaining milliseconds until deadline_us, given the current
 * time now_us. Pure, host-testable.
 *
 * @return true with *out_remaining_ms set (the remaining time, truncated to
 *         whole milliseconds) if now_us < deadline_us; false (deadline
 *         already reached/passed) if now_us >= deadline_us — the caller
 *         MUST treat false as an immediate timeout, not call
 *         bb_lock_cond_wait() again.
 */
bool bb_bqueue_deadline_remaining_ms(uint64_t deadline_us, uint64_t now_us, uint32_t *out_remaining_ms);

#ifdef __cplusplus
}
#endif
