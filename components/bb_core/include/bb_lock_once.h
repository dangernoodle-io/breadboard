#pragma once

// bb_lock_once — shared "lazily bb_lock_init() exactly once" helper.
//
// Composing a fallible bb_lock_init() with the plain bb_once_run() latches
// DONE unconditionally, permanently replaying one transient bb_lock_init()
// failure forever (the exact latent defect documented in bb_lock.h and
// bb_once.h). bb_lock_once_ensure() instead composes bb_lock_init() with
// bb_once_run_fallible() (B1-524): success latches DONE, failure resets to
// IDLE so a later caller genuinely retries — mirroring the 3 sites B1-524
// already migrated (bb_wifi_ping, bb_timer, bb_mdns) as a single reusable
// helper instead of re-hand-rolling the same ctx-pack/adapter idiom at every
// call site.
//
// Usage:
//   static bb_once_t s_once = BB_ONCE_INIT;
//   static bb_lock_t s_lock;
//   bb_lock_config_t cfg = { .name = "my_lock" };
//   bb_err_t rc = bb_lock_once_ensure(&s_once, &cfg, &s_lock);
//   if (rc != BB_OK) { return rc; } // this call's own attempt (or the
//                                   // racer whose attempt it observed)
//                                   // failed; caller may retry later.

#include "bb_core.h"
#include "bb_once.h"
#include "bb_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ensure *lock is bb_lock_init()'d exactly once across all callers sharing
// *once, retrying (not permanently latching) a transient bb_lock_init()
// failure. Returns BB_OK once *lock is (now, or already) successfully
// initialized; BB_ERR_NO_MEM if this call's own bb_lock_init() attempt (or
// the concurrent racer whose attempt it observed) failed.
//
// Defense-in-depth: after bb_once_run_fallible() reports success, this also
// re-checks bb_lock_is_initialized(lock) before returning BB_OK. This is
// provably redundant, not load-bearing: bb_once_run_fallible() uses seq_cst
// atomics, and bb_lock_init()'s release-store into bb_lock_initialized is
// paired with bb_lock_is_initialized()'s acquire-load, so happens-before is
// already established — the C11 memory model rules out a losing racer
// observing a stale view. Unlike bb_wifi_ping's "!s_ping_mutex" recheck
// (platform/espidf/bb_wifi/bb_wifi.c), which guards genuinely non-atomic
// plain globals and is load-bearing there, this recheck is kept only as a
// cheap invariant assertion belt-and-suspenders alongside the once guard's
// own return value.
bb_err_t bb_lock_once_ensure(bb_once_t *once, const bb_lock_config_t *cfg, bb_lock_t *lock);

#ifdef __cplusplus
}
#endif
