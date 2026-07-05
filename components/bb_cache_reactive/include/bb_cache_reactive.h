#pragma once

// bb_cache_reactive — change-driven observer layer on top of bb_cache
// (KB#629 canonical bb_cache family; B1-589).
//
// PR-4b scope (pinned — do not re-litigate without a new decision):
//   - Observers register interest in a specific key or ALL keys (key==NULL).
//   - bb_cache_reactive_update() wraps bb_cache_update(): after a successful
//     write that actually CHANGED the stored value, every matching
//     observer's on_change is fired with the freshly-serialized {ts_ms,data}
//     envelope, ts_ms lifted out and json pointing at the "data" bytes only.
//
// A2 scope (B1-592 A2 — reactive triad, on top of A1's bb_cache_delete):
//   - bb_cache_reactive_register() wraps bb_cache_register(): on a
//     FIRST-TIME registration success (not the idempotent already-registered
//     case), every matching observer's on_register is fired with the key.
//   - bb_cache_reactive_delete() wraps bb_cache_delete(): on success, every
//     matching observer's on_remove is fired with the key, AFTER the entry
//     has been freed.
//   - Both firings use the same snapshot-then-notify shape as on_change
//     (see the Reentrancy note below).
//
// Reentrancy (load-bearing, B1-589; extended B1-592 A2 for the full triad):
//   The matching observer list is snapshotted under an internal lock, the
//   lock is released, and only THEN are callbacks invoked (mirrors
//   bb_cache_foreach). This holds symmetrically for all three callbacks:
//   on_change, on_register, and on_remove. Each may safely call bb_cache_* /
//   bb_cache_reactive_observe() (a fresh registration) or reentrantly drive
//   bb_cache_reactive_register()/bb_cache_reactive_delete() for a DIFFERENT
//   key (bb_cache_register_ex()'s single-lock-acquisition find-or-init makes
//   same-key first-time detection atomic, so a reentrant call for a
//   DIFFERENT key never races the in-flight registration/deletion this
//   callback was fired from). Callbacks MUST NOT call back into
//   bb_cache_reactive_update()/_register()/_delete() for the SAME key they
//   were fired for -- that reintroduces the recursion this snapshot-then-
//   notify shape exists to avoid. This layer never holds a raw bb_cache
//   entry pointer -- it only ever calls bb_cache's public API.
//
// Kconfig-gated (CONFIG_BB_CACHE_REACTIVE_ENABLE, default n): when disabled,
// bb_cache_reactive_observe() is a static-inline no-op (BB_ERR_UNSUPPORTED)
// and bb_cache_reactive_update() is a static-inline passthrough to plain
// bb_cache_update() -- callers may invoke either unconditionally without an
// #ifdef, at zero cost when the component is compiled out (pay-for-what-
// you-use, e.g. tight boards like c3).

#include "bb_core.h"
#include "bb_cache.h"

// ---------------------------------------------------------------------------
// Kconfig bridge (pattern from bb_clock.h / CLAUDE.md)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_CACHE_REACTIVE_ENABLE
#define BB_CACHE_REACTIVE_ENABLE CONFIG_BB_CACHE_REACTIVE_ENABLE
#endif
#ifdef CONFIG_BB_CACHE_REACTIVE_MAX_OBSERVERS
#define BB_CACHE_REACTIVE_MAX_OBSERVERS CONFIG_BB_CACHE_REACTIVE_MAX_OBSERVERS
#endif
#ifdef CONFIG_BB_CACHE_REACTIVE_PAYLOAD_MAX
#define BB_CACHE_REACTIVE_PAYLOAD_MAX CONFIG_BB_CACHE_REACTIVE_PAYLOAD_MAX
#endif
#endif
#ifndef BB_CACHE_REACTIVE_ENABLE
#define BB_CACHE_REACTIVE_ENABLE 0
#endif
#ifndef BB_CACHE_REACTIVE_MAX_OBSERVERS
#define BB_CACHE_REACTIVE_MAX_OBSERVERS 8
#endif
#ifndef BB_CACHE_REACTIVE_PAYLOAD_MAX
#define BB_CACHE_REACTIVE_PAYLOAD_MAX 512
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Observer callback triad (PINNED signatures, B1-589)
// ---------------------------------------------------------------------------

// New key registered. Fires exactly once, iff the registration is the
// key's FIRST-TIME registration via bb_cache_reactive_register() --
// re-registering an already-registered key is idempotent (matches
// bb_cache_register()) and does NOT re-fire.
typedef void (*bb_cache_on_register_fn)(const char *key, void *ctx);

// Value changed. json/len point at the "data" object bytes only (ts_ms is
// lifted out of the {ts_ms,data} envelope and passed separately) -- the
// range is NOT guaranteed NUL-terminated; callers must use len.
typedef void (*bb_cache_on_change_fn)(const char *key, const char *json,
                                       size_t len, int64_t ts_ms, void *ctx);

// Key deleted. Fires exactly once per successful bb_cache_reactive_delete(),
// AFTER the entry has been freed -- the value is no longer readable at fire
// time (only the key string, captured before the free, is available to the
// callback).
typedef void (*bb_cache_on_remove_fn)(const char *key, void *ctx);

// Config-struct registration (single struct arg, no _ex -- see the
// "API variant naming" convention in CLAUDE.md).
//
//   key         — NULL observes every key; non-NULL observes exactly that
//                 key (copied, up to BB_CACHE_KEY_MAX-1 chars).
//   on_register — fired synchronously after a first-time bb_cache_reactive_register().
//   on_change   — fired synchronously after a changed bb_cache_reactive_update().
//   on_remove   — fired synchronously after a successful bb_cache_reactive_delete().
//   ctx         — passed through to every callback unchanged.
typedef struct {
    const char              *key;
    bb_cache_on_register_fn  on_register;
    bb_cache_on_change_fn    on_change;
    bb_cache_on_remove_fn    on_remove;
    void                    *ctx;
} bb_cache_reactive_observer_t;

#if BB_CACHE_REACTIVE_ENABLE

// Register an observer. Fixed-size pool (BB_CACHE_REACTIVE_MAX_OBSERVERS).
// Returns BB_ERR_INVALID_ARG if cfg is NULL or cfg->key is over-length.
// Returns BB_ERR_NO_SPACE if the observer pool is full.
bb_err_t bb_cache_reactive_observe(const bb_cache_reactive_observer_t *cfg);

// Write-through to bb_cache_update(): performs the plain update (req's own
// out_changed, if set, is still honored) and, iff the write actually
// changed the stored value, fetches the key's serialized {ts_ms,data}
// envelope and fires every matching observer's on_change with ts_ms lifted
// out and json pointing at the "data" bytes.
// Returns whatever bb_cache_update() returns.
bb_err_t bb_cache_reactive_update(const bb_cache_update_t *req);

// Write-through to bb_cache_register(): performs the plain registration
// and, iff this is the key's FIRST-TIME registration (bb_cache_register()
// is idempotent -- re-registering an already-registered key returns BB_OK
// without creating a duplicate entry and does NOT count as first-time),
// fires every matching observer's on_register with the key.
// Returns whatever bb_cache_register() returns.
bb_err_t bb_cache_reactive_register(const bb_cache_config_t *cfg);

// Write-through to bb_cache_delete(): performs the plain delete and, on
// success, fires every matching observer's on_remove with the key AFTER the
// entry has been freed -- observers must not assume the value is still
// readable (bb_cache_get_serialized/bb_cache_get_raw will return
// BB_ERR_NOT_FOUND for the key by the time on_remove runs).
// Returns whatever bb_cache_delete() returns.
bb_err_t bb_cache_reactive_delete(const char *key);

#else

static inline bb_err_t bb_cache_reactive_observe(const bb_cache_reactive_observer_t *cfg)
{
    (void)cfg;
    return BB_ERR_UNSUPPORTED;
}

static inline bb_err_t bb_cache_reactive_update(const bb_cache_update_t *req)
{
    return bb_cache_update(req);
}

static inline bb_err_t bb_cache_reactive_register(const bb_cache_config_t *cfg)
{
    return bb_cache_register(cfg);
}

static inline bb_err_t bb_cache_reactive_delete(const char *key)
{
    return bb_cache_delete(key);
}

#endif // BB_CACHE_REACTIVE_ENABLE

#ifdef BB_CACHE_REACTIVE_TESTING
#include <stdbool.h>
void bb_cache_reactive_reset_for_test(void);
void bb_cache_reactive_set_envelope_split_for_test(
    bool (*fn)(const char *, int, const char **, size_t *, const char **, size_t *));
#endif

#ifdef __cplusplus
}
#endif
