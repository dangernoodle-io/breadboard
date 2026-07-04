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
//   - on_register / on_remove are ACCEPTED and STORED but never invoked in
//     this PR — reserved for a later PR (on_remove needs B1-592 delete/
//     eviction; on_register needs a new-key-notify hook). Callers may safely
//     register them now; behavior is a documented no-op until then.
//
// Reentrancy (load-bearing, B1-589):
//   The matching observer list is snapshotted under an internal lock, the
//   lock is released, and only THEN are callbacks invoked (mirrors
//   bb_cache_foreach). on_change callbacks may safely call bb_cache_* /
//   bb_cache_reactive_observe() (a fresh registration), but MUST NOT call
//   back into bb_cache_reactive_update() for the same key from within the
//   callback -- that reintroduces the recursion this snapshot-then-notify
//   shape exists to avoid. This layer never holds a raw bb_cache entry
//   pointer -- it only ever calls bb_cache's public API.
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

// New key registered. ACCEPTED but NOT fired in PR-4b (reserved).
typedef void (*bb_cache_on_register_fn)(const char *key, void *ctx);

// Value changed. json/len point at the "data" object bytes only (ts_ms is
// lifted out of the {ts_ms,data} envelope and passed separately) -- the
// range is NOT guaranteed NUL-terminated; callers must use len.
typedef void (*bb_cache_on_change_fn)(const char *key, const char *json,
                                       size_t len, int64_t ts_ms, void *ctx);

// Key deleted/evicted. ACCEPTED but NOT fired in PR-4b (reserved; needs
// B1-592 delete/eviction).
typedef void (*bb_cache_on_remove_fn)(const char *key, void *ctx);

// Config-struct registration (single struct arg, no _ex -- see the
// "API variant naming" convention in CLAUDE.md).
//
//   key         — NULL observes every key; non-NULL observes exactly that
//                 key (copied, up to BB_CACHE_KEY_MAX-1 chars).
//   on_register — reserved, stored but never invoked in this PR.
//   on_change   — fired synchronously after a changed bb_cache_reactive_update().
//   on_remove   — reserved, stored but never invoked in this PR.
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
