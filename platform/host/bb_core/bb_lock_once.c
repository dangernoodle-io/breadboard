// bb_lock_once — portable implementation of bb_lock_once_ensure(). Compiled
// once, linked into both the host and ESP-IDF builds (host: picked up
// automatically via the platform/host/<component> source glob; ESP-IDF:
// added explicitly to components/bb_core/CMakeLists.txt SRCS) — same
// cross-platform-shared-TU pattern as bb_claim.c/bb_lock_stats.c in this
// directory.

#include "bb_lock_once.h"

typedef struct {
    const bb_lock_config_t *cfg;
    bb_lock_t *lock;
} bb_lock_once_ctx_t;

static bool bb_lock_once_adapter(void *ctx)
{
    bb_lock_once_ctx_t *c = ctx;
    // bb_lock_init() failure is not host-reproducible (same rationale as
    // bb_lock_init()'s own already-excluded failure branch,
    // platform/host/bb_core/bb_lock.c:28-35).
    return bb_lock_init(c->cfg, c->lock) == BB_OK;  // LCOV_EXCL_BR_LINE
}

bb_err_t bb_lock_once_ensure(bb_once_t *once, const bb_lock_config_t *cfg, bb_lock_t *lock)
{
    if (!once || !lock) {
        return BB_ERR_INVALID_ARG;
    }
    bb_lock_once_ctx_t ctx = { .cfg = cfg, .lock = lock };
    bool ok = bb_once_run_fallible(once, bb_lock_once_adapter, &ctx);
    // LCOV_EXCL_START -- neither branch below is host-reproducible: `!ok`
    // requires bb_lock_init() to actually fail (excluded in bb_lock_init()
    // itself, see the comment above). `!bb_lock_is_initialized(lock)` can
    // never actually trigger here either -- bb_once_run_fallible() uses
    // seq_cst atomics, and bb_lock_init() stores bb_lock_initialized with
    // release ordering while bb_lock_is_initialized() loads it with acquire,
    // so happens-before is already established and the C11 memory model
    // rules out a stale view. Unlike bb_wifi_ping's "!s_ping_mutex" recheck
    // (platform/espidf/bb_wifi/bb_wifi.c), which guards genuinely
    // non-atomic plain globals and is load-bearing there, this recheck is
    // provably redundant given the atomics above -- kept anyway as a cheap
    // invariant assertion, not because it can fail. HIL-only equivalent
    // tracked as B1-692.
    if (!ok || !bb_lock_is_initialized(lock)) {
        return BB_ERR_NO_MEM;
    }
    // LCOV_EXCL_STOP
    return BB_OK;
}
