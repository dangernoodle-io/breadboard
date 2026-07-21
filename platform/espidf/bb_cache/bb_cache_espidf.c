// Must come before any system header on Linux — glibc gates PTHREAD_MUTEX_RECURSIVE
// on _GNU_SOURCE (or _XOPEN_SOURCE >= 500). macOS exposes it unconditionally.
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "bb_cache.h"
#if BB_CACHE_SWEEP_ENABLE
#include "bb_timer.h"
#endif
#include "bb_cache_internal.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_core.h"
#include "bb_mem.h"
#include "bb_clock.h"
#include "bb_str.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

static const char *TAG = "bb_cache";

// ---------------------------------------------------------------------------
// Registry entry
// ---------------------------------------------------------------------------

typedef struct {
    char                 key[BB_CACHE_KEY_MAX]; // key[0] == '\0' = slot free
    const void         *(*snapshot)(void); // NULL = owned mode
    void                *owned;           // heap buffer in owned mode; NULL in getter mode
    size_t               size;            // sizeof owned struct (owned mode only)
    bool                 has_value;       // owned mode: true once bb_cache_update has
                                           // copied in a value at least once (guards a
                                           // false-negative memcmp against a zero-init buf)
    bool                 fallback_seeded; // owned+fallback (PR-4a-0) only: true once the
                                           // cold-start snapshot() seed has run. Deliberately
                                           // separate from has_value -- has_value stays false
                                           // across a seed so the first REAL write still
                                           // reports changed=true unconditionally (see
                                           // maybe_seed_fallback()).
    bb_cache_serialize_fn fn;
    pthread_mutex_t      lock;            // process-lifetime — see the tombstone/generation
                                           // invariant documented above find_entry_locked_ref().
                                           // NEVER pthread_mutex_destroy'd or re-pthread_mutex_init'd
                                           // once ensure_init() has created it.
    bb_cache_flags_t     flags;           // BB_CACHE_FLAG_* bitmask
    char                *cached_json;     // memoized serialized "data" bytes (NULL = none yet)
    size_t               cached_len;      // strlen of cached_json
    bool                 dirty;           // true = cached_json stale, re-serialize on next get
    // Envelope sample-time (B1-570 PR-3): owned mode is stamped in
    // bb_cache_update() right after the memcpy; getter mode is stamped each
    // time snapshot() is invoked (serialize_locked / bb_cache_get_serialized).
    // Producers no longer emit their own ts_ms — bb_cache owns it and wraps
    // every serialize point ({"ts_ms":N,"data":{...}}).
    int64_t              ts_ms;
    // Age-out eviction (B1-592 A3). policy defaults to BB_CACHE_EVICT_PINNED
    // (0) whenever cfg->eviction is zero-initialized -- preserves the
    // pre-A3 never-auto-free behavior. stale_age_ms/evict_age_ms are only
    // meaningful when policy == BB_CACHE_EVICT_AGE_OUT (see
    // bb_cache_register()'s AGE_OUT validation).
    bb_cache_evict_policy_t policy;
    uint32_t             stale_age_ms;
    uint32_t             evict_age_ms;
    // Tombstone/generation guard (B1-592 firmware-review fix): bumped every
    // time this slot transitions free -> in-use (bb_cache_register populating
    // a freed or never-used slot) AND every time it transitions in-use ->
    // free (bb_cache_delete). A reader that captures (entry pointer,
    // generation) under s_reg_lock and re-validates BOTH key and generation
    // after acquiring e->lock is guaranteed to either observe the exact
    // incarnation it looked up, or detect the mismatch and bail with
    // BB_ERR_NOT_FOUND -- see find_entry_locked_ref()'s doc comment.
    uint32_t             generation;
    // Monotonic per-key write counter (B1-767 PR-3): bumped exactly once per
    // successful OWNED-mode bb_cache_update() write, under e->lock. Resets
    // to 0 on register/free (see free_entry_locked() and bb_cache_register()'s
    // cfg->out_first_time-reporting fresh-slot init). Deliberately distinct
    // from `generation` above -- see the header-level doc comment on
    // bb_cache.h's state_version section for the full contrast.
    uint32_t             state_version;
} bb_cache_entry_t;

static bb_cache_entry_t s_entries[BB_CACHE_MAX_TOPICS];
static pthread_mutex_t  s_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t   s_init_once = PTHREAD_ONCE_INIT;

// One-way evict-notify hook (B1-592 A3, revises A2). Fired by bb_cache_delete()
// (explicit delete AND the age-out eviction path, which funnels through
// bb_cache_delete_if_generation()) after a slot has actually been freed,
// outside all bb_cache locks. Single-slot contract, deliberately not an
// N-observer registry (see bb_cache_internal.h): exactly one composer
// installs a hook here at a time -- currently bb_mdns_cache (B1-1118, which
// replaced the former installer, bb_cache_reactive).
//
// _Atomic (firmware-review fix, revises A3): the installer's
// bb_cache_set_evict_notify_fn() call writes this from its own thread, and
// it is read here on every delete/eviction, from whichever thread happens to
// be deleting/evicting -- a plain `void (*)()` global read/written from
// multiple threads with no synchronization is a data race. Relaxed ordering
// is sufficient: this is a one-shot install-then-fire pointer with no other
// memory it needs to publish-with (the callback itself takes its own
// locks), so we only need atomicity of the pointer read/write, not a
// happens-before edge to any other data.
static _Atomic(void (*)(const char *key)) s_evict_notify_fn = NULL;

void bb_cache_set_evict_notify_fn(void (*fn)(const char *key))
{
    atomic_store_explicit(&s_evict_notify_fn, fn, memory_order_relaxed);
}

// One-way write-notify hook (B1-767 PR-3): fired by bb_cache_update()'s
// owned-write path, OUTSIDE e->lock, after every successful owned write --
// same _Atomic-relaxed one-shot install-then-fire idiom as
// s_evict_notify_fn above (see its doc comment for the full race
// rationale). Deliberately NOT bb_callback_slot -- that slot type is
// non-atomic and would reintroduce the exact cross-thread race this
// idiom exists to avoid.
static _Atomic(bb_cache_write_notify_fn) s_write_notify_fn = NULL;

void bb_cache_set_write_notify_fn(bb_cache_write_notify_fn fn)
{
    atomic_store_explicit(&s_write_notify_fn, fn, memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline bool is_plain_getter(const bb_cache_entry_t *e) { return e->snapshot && !e->owned; }

// Per-slot mutexes are created exactly once, here, and are NEVER destroyed or
// re-initialized for the life of the process -- see the tombstone/generation
// invariant documented on find_entry_locked_ref() below. This is what makes
// it safe for a reader to hold a raw entry pointer across the window where
// s_reg_lock is not held: pthread_mutex_lock(&e->lock) can never target a
// destroyed mutex, no matter how many delete/register cycles the slot has
// been through.
//
// pthread_once (rather than a hand-rolled double-checked-locking bool) is the
// idiomatic POSIX exactly-once primitive: it guarantees do_init() runs
// exactly once across however many threads race ensure_init(), with no
// separate "already initialized" branch to (mis-)test or (mis-)synchronize.
static void do_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        s_entries[i].key[0] = '\0';
        s_entries[i].generation = 0;
        pthread_mutex_init(&s_entries[i].lock, &attr);
    }
    pthread_mutexattr_destroy(&attr);
}

static void ensure_init(void)
{
    pthread_once(&s_init_once, do_init);
}

static bb_cache_entry_t *find_entry(const char *key)
{
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] != '\0' && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

// Runtime lookup helper: takes s_reg_lock for the scan, releases it before
// returning. Used only by bb_cache_exists(), which needs presence at a single
// point in time and never dereferences the returned pointer's contents --
// every OTHER caller in this file must use find_entry_locked_ref() below
// instead, so it can re-validate identity after taking e->lock.
static bb_cache_entry_t *find_entry_locked(const char *key)
{
    pthread_mutex_lock(&s_reg_lock);
    bb_cache_entry_t *e = find_entry(key);
    pthread_mutex_unlock(&s_reg_lock);
    return e;
}

// (entry, generation) pair captured under s_reg_lock by find_entry_locked_ref().
// entry == NULL means "not found".
typedef struct {
    bb_cache_entry_t *entry;
    uint32_t          generation;
} entry_ref_t;

// Runtime lookup helper: takes s_reg_lock for the scan, captures both the
// slot pointer AND its current generation counter, releases s_reg_lock, then
// returns. The bb_cache_entry_t slots themselves are a fixed static array
// (s_entries[]) and are never freed/relocated -- a returned pointer's storage
// stays valid for the process lifetime (see ensure_init()'s mutex-lifetime
// comment), so dereferencing e->lock itself is never a use-after-free.
//
// BUT entries are no longer add-only: bb_cache_delete() can free a slot's
// contents, bump its generation, and mark it free (key[0] = '\0') the moment
// this function releases s_reg_lock, and a subsequent bb_cache_register() can
// re-init that same slot -- for the SAME key (a fresh incarnation) or a
// DIFFERENT key (cross-key slot reuse) -- bumping the generation again.
//
// Tombstone/generation invariant: every caller MUST, immediately after
// acquiring e->lock (and before reading or writing any other field), call
// entry_matches_locked(e, key, ref.generation) and bail with
// BB_ERR_NOT_FOUND on a mismatch. This is safe because:
//   - bb_cache_delete() holds s_reg_lock across its ENTIRE operation
//     (find + teardown + generation bump + slot-free), nested with e->lock
//     held only for the teardown+bump sub-step.
//   - bb_cache_register() likewise holds s_reg_lock across its entire
//     operation (find-free-slot + populate + generation bump) and never
//     touches e->lock at all.
// So a reader blocked between find_entry_locked_ref()'s release of
// s_reg_lock and its own pthread_mutex_lock(&e->lock) can only ever observe,
// once it acquires e->lock, either: (a) the SAME incarnation it looked up
// (key and generation both match -- safe to proceed), or (b) a LATER
// incarnation, whether a fresh write of the same key or a completely
// different key (key and/or generation mismatch -- must bail). It can never
// observe a half-torn-down or half-initialized slot, because neither
// bb_cache_delete() nor bb_cache_register() ever releases s_reg_lock
// mid-operation.
//
// Callers must NOT already hold s_reg_lock (non-recursive) or any entry's
// e->lock (lock ordering: s_reg_lock is always acquired/released before an
// entry's own lock, never nested inside it -- bb_cache_delete is the sole
// exception, documented at its call site).
static entry_ref_t find_entry_locked_ref(const char *key)
{
    entry_ref_t ref = { .entry = NULL, .generation = 0 };
    pthread_mutex_lock(&s_reg_lock);
    bb_cache_entry_t *e = find_entry(key);
    if (e) {
        ref.entry      = e;
        ref.generation = e->generation;
    }
    pthread_mutex_unlock(&s_reg_lock);
    return ref;
}

// Re-validation predicate -- call under e->lock immediately after acquiring
// it, using the (key, generation) pair captured by find_entry_locked_ref().
// See that function's doc comment for the full invariant.
static inline bool entry_matches_locked(const bb_cache_entry_t *e, const char *key,
                                         uint32_t generation)
{
    if (e->key[0] == '\0') return false;             // tombstoned, never re-registered
    if (e->generation != generation) return false;   // stale incarnation (delete and/or register since capture)
    // Every occupied-slot transition (bb_cache_delete() or bb_cache_register()
    // filling a free slot) bumps e->generation by exactly one -- see
    // find_entry_locked_ref()'s and bb_cache_delete()'s doc comments -- and
    // the counter is monotonic (never reset). So the only way this reader's
    // captured generation can still equal the slot's CURRENT generation is if
    // there has been NO occupied-slot transition since the capture at all --
    // i.e. it is provably still the exact same incarnation, which by
    // construction still holds the exact same key. A generation match paired
    // with a DIFFERENT key would require the counter to wrap all the way back
    // to the captured value (2^32 transitions on one slot) -- unreachable in
    // any real run. LCOV_EXCL_BR_LINE
    return strcmp(e->key, key) == 0;
}

// Tear down an entry's owned resources, mirroring bb_cache_reset_for_test's
// per-entry teardown EXACTLY. Caller holds e->lock. Does NOT touch e->key,
// e->generation, or the mutex itself -- those are the caller's
// responsibility (bb_cache_delete bumps generation and clears key itself,
// from its own scope, after this helper returns).
static void free_entry_locked(bb_cache_entry_t *e)
{
    bb_mem_free(e->owned);
    e->owned = NULL;
    if (e->cached_json) bb_json_free_str(e->cached_json);
    e->cached_json = NULL;
    e->cached_len = 0;
    e->dirty = true;
    e->has_value = false;
    e->fallback_seeded = false;
    e->state_version = 0;  // B1-767 PR-3: resets on free, matches register's reset
}

#ifdef BB_CACHE_TESTING
// Test-only reentrancy injection point (B1-592 firmware-review fix,
// line-coverage follow-up): the multi-threaded race test
// (test_bb_cache_delete_reader_race_never_poisons_or_crashes) exercises the
// tombstone/generation-mismatch branch below via genuine thread-scheduling
// races, which is schedule-dependent and does not reliably hit every call
// site on every host/CI combination (observed: reliably hit on macOS,
// not on the Linux CI runner within a bounded iteration count). This hook
// lets a single-threaded host test deterministically reproduce the exact
// interleaving instead: set via bb_cache_test_set_race_hook(), it fires
// exactly once, synchronously, at each of the five call sites below --
// immediately after find_entry_locked_ref() captures (entry, generation) and
// BEFORE the entry's own lock is taken -- so a hook body that deletes and
// re-registers the same key deterministically bumps the generation and
// guarantees the caller observes a mismatch on its next lock acquisition.
static void (*s_test_race_hook)(const char *key) = NULL;

void bb_cache_test_set_race_hook(void (*hook)(const char *key))
{
    s_test_race_hook = hook;
}

static void fire_test_race_hook(const char *key)
{
    if (!s_test_race_hook) return;
    void (*h)(const char *) = s_test_race_hook;
    s_test_race_hook = NULL; // one-shot: never re-arms across calls
    h(key);
}
#define BB_CACHE_TEST_RACE_POINT(key) fire_test_race_hook(key)

// Injectable clock (B1-592 A3): lets a single-threaded host test advance
// "now" deterministically to exercise age-out eviction boundaries without
// real sleeps. Same idiom as s_test_race_hook above -- reset via
// bb_cache_reset_for_test() is NOT required (tests call
// bb_cache_test_set_clock(NULL) themselves to restore the real clock; see
// test_bb_cache_evict.c).
static uint64_t (*s_test_clock_hook)(void) = NULL;

void bb_cache_test_set_clock(uint64_t (*fn)(void))
{
    s_test_clock_hook = fn;
}
#else
#define BB_CACHE_TEST_RACE_POINT(key) ((void)0)
#endif

// Forward declaration -- defined below, next to the public bb_cache_delete()
// it shares its teardown with (see that pairing's doc comments). Needed here
// because evict_if_aged_out_locked() (LAZY floor + SWEEP backstop, both
// defined ahead of bb_cache_delete() in this file) calls it.
static bb_err_t bb_cache_delete_if_generation(const char *key, uint32_t expected_gen);

// Canonical "now" for all age-out computations in this file -- routes
// through the injectable test clock when BB_CACHE_TESTING is compiled in and
// a hook is installed, otherwise the real monotonic clock (bb_clock.h).
static inline uint64_t cache_now_ms64(void)
{
#ifdef BB_CACHE_TESTING
    if (s_test_clock_hook) return s_test_clock_hook();
#endif
    return bb_clock_now_ms64();
}

// Owned+fallback cold-start seed (PR-4a-0). Called under e->lock from every
// read/serialize path that is about to hand out e->owned bytes.
//
// If the entry has BOTH an owned buffer (e->owned != NULL) AND a fallback
// getter (e->snapshot != NULL) -- the owned+fallback tri-state -- and no real
// write has landed yet (e->has_value == false), invoke snapshot() ONCE and
// copy its bytes into the owned buffer so a reader never sees an empty/zero
// cold-start value. `fallback_seeded` guards against invoking snapshot()
// again on a subsequent read while still unpopulated.
//
// Deliberately does NOT set has_value: that flag is reserved for
// bb_cache_update's memcmp change-detect guard, so the entry's first REAL
// write still reports changed=true unconditionally (identical to plain
// owned mode), never comparing against these seeded bytes. The seed is a
// boot-race bridge, not a "change" -- it must never look like a write to the
// change-detection or (future PR-4b) observer-notify machinery.
//
// No-op for plain owned mode (e->snapshot == NULL). Every call site only
// invokes this function when e->owned != NULL (the "not a plain getter"
// branch) -- plain getter mode (owned == NULL) never reaches here -- so
// e->snapshot != NULL at this point unambiguously means owned+fallback.
static void maybe_seed_fallback(bb_cache_entry_t *e)
{
    if (!e->snapshot) return;   // plain owned mode, not owned+fallback
    if (e->has_value || e->fallback_seeded) return;  // already real or seeded

    const void *snap = e->snapshot();
    if (snap) {
        memcpy(e->owned, snap, e->size);
        e->ts_ms = (int64_t)cache_now_ms64();  // seed IS a sample
    }
    e->fallback_seeded = true;  // never retry, even if snapshot() returned NULL
}

// LAZY age-out eviction floor (B1-592 A3). Called under e->lock immediately
// after entry_matches_locked() succeeds, from every read call site
// (serialize_locked, bb_cache_get_serialized, bb_cache_get_raw) and from the
// SWEEP backstop's sweep_cb() below.
//
// Returns true if the entry was evicted -- e->lock has ALREADY been released
// by this function and the caller must not touch e any further; the caller's
// own operation must return/report BB_ERR_NOT_FOUND (the "read misses"
// contract). Returns false (e->lock still held) if the entry is not evicted,
// so the caller proceeds with its normal read under the still-held lock.
//
// NEVER acquires s_reg_lock while holding e->lock -- the AB-BA deadlock this
// design forbids (bb_cache_delete()/bb_cache_delete_if_generation() hold
// s_reg_lock across their entire operation, nested with e->lock only for the
// teardown sub-step; taking s_reg_lock here while already holding e->lock
// would invert that order). Instead: capture the key AND generation into
// stack locals, release e->lock, THEN call bb_cache_delete_if_generation() --
// which does a FRESH find under s_reg_lock and only tears down the entry if
// its CURRENT generation still matches the one captured here.
//
// Identity-keyed, not string-keyed (firmware-review fix, revises A3): a
// string-keyed bb_cache_delete(key_copy) here would delete+notify whatever
// incarnation find_entry(key) turns up at the moment it runs -- including a
// BRAND-NEW registration of the same key that landed in the window between
// this function releasing e->lock and the delete actually running (two
// evictors racing, or an evictor racing a same-key bb_cache_register() reuse).
// Passing the generation captured under e->lock lets
// bb_cache_delete_if_generation() detect that mismatch and skip the wrong
// incarnation instead of freeing/notifying it.
static bool evict_if_aged_out_locked(bb_cache_entry_t *e)
{
    if (e->policy != BB_CACHE_EVICT_AGE_OUT) return false;

    uint64_t age = cache_now_ms64() - (uint64_t)e->ts_ms;
    if (bb_cache_evaluate_age(age, e->stale_age_ms, e->evict_age_ms) != BB_CACHE_ENTRY_EVICT) {
        return false;
    }

    char key_copy[BB_CACHE_KEY_MAX];
    bb_strlcpy(key_copy, e->key, sizeof(key_copy));
    uint32_t gen = e->generation;
    pthread_mutex_unlock(&e->lock);

    // Test-only deterministic race window (BB_CACHE_TESTING): fires here,
    // AFTER releasing e->lock and capturing (key, generation) but BEFORE the
    // identity-keyed delete below -- a hook body that deletes and
    // re-registers this same key deterministically reproduces the
    // double-evictor/same-key-reuse race the generation guard defends
    // against (see test_bb_cache_evict.c).
    BB_CACHE_TEST_RACE_POINT(key_copy);

    bb_cache_delete_if_generation(key_copy, gen);  // return value ignored: mismatch/not-found is a silent no-op
    return true;
}

// Shared owned-copy body for bb_cache_get_raw and bb_cache_snapshot
// (B1-767 PR-3 firmware-review consolidation fix): both need the identical
// not-getter-mode check, cap check, seed-if-fallback, and a single memcpy of
// the owned struct -- differing only in whether the caller also wants
// size/state_version captured atomically with the copy (bb_cache_snapshot
// does; bb_cache_get_raw doesn't). Caller holds e->lock (already
// re-validated via entry_matches_locked + evict_if_aged_out_locked) on
// entry; this function ALWAYS releases e->lock before returning, on every
// path -- neither caller may touch e->lock again after calling this.
//   op          -- "get_raw" or "snapshot", used ONLY in the BB_ERR_NO_SPACE
//                  log line, so each caller's distinct log text (and log
//                  behavior) is preserved exactly.
//   out_size    -- nullable; captures e->size under the SAME lock as the
//                  memcpy (bb_cache_snapshot's out->size). NULL for
//                  bb_cache_get_raw, which has no size output.
//   out_version -- nullable; captures e->state_version under the SAME lock
//                  as the memcpy -- this atomicity is exactly why
//                  bb_cache_snapshot can't just call bb_cache_get_raw and
//                  then bb_cache_state_version separately (see
//                  bb_cache_snapshot's doc comment). NULL for
//                  bb_cache_get_raw.
static bb_err_t copy_owned_and_capture_locked(bb_cache_entry_t *e, const char *op, const char *key,
                                               void *buf, size_t cap,
                                               size_t *out_size, uint32_t *out_version)
{
    if (!e->owned) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_INVALID_STATE;
    }
    if (cap < e->size) {
        pthread_mutex_unlock(&e->lock);
        bb_log_w(TAG, "%s '%s': buf too small (%zu < %zu)", op, key, cap, e->size);
        return BB_ERR_NO_SPACE;
    }
    maybe_seed_fallback(e);  // owned+fallback: seed if still unpopulated
    memcpy(buf, e->owned, e->size);
    if (out_size) *out_size = e->size;
    if (out_version) *out_version = e->state_version;
    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

// Serialize entry contents into obj under the entry's lock.
// Entry must not be NULL. Caller holds no lock before calling.
// key/generation are the identity captured by find_entry_locked_ref(); this
// function re-validates them immediately after taking e->lock (tombstone/
// generation guard, see find_entry_locked_ref()'s doc comment) and returns
// BB_ERR_NOT_FOUND on a mismatch WITHOUT reading or writing any other field.
// out_ts_ms, when non-NULL, receives the entry's envelope sample-time: for
// getter-mode entries this call stamps ts_ms = now (the read IS the sample);
// for owned-mode entries ts_ms was already stamped by the last bb_cache_update.
static bb_err_t serialize_locked(bb_cache_entry_t *e, const char *key, uint32_t generation,
                                  bb_json_t obj, int64_t *out_ts_ms)
{
    BB_CACHE_TEST_RACE_POINT(key);
    pthread_mutex_lock(&e->lock);

    if (!entry_matches_locked(e, key, generation)) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NOT_FOUND;
    }

    if (evict_if_aged_out_locked(e)) {
        return BB_ERR_NOT_FOUND;
    }

    const void *snap;
    if (is_plain_getter(e)) {
        // Plain getter mode: no owned buffer, always pull through.
        snap = e->snapshot();
        e->ts_ms = (int64_t)cache_now_ms64();
    } else {
        // Plain owned mode, or owned+fallback (seed if still unpopulated).
        maybe_seed_fallback(e);
        snap = e->owned;
    }

    if (!snap) {
        pthread_mutex_unlock(&e->lock);
        bb_log_w(TAG, "key '%s': no snapshot available", e->key);
        return BB_ERR_INVALID_STATE;
    }

    e->fn(obj, snap);
    if (out_ts_ms) *out_ts_ms = e->ts_ms;
    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_cache_register(const bb_cache_config_t *cfg)
{
    if (cfg && cfg->out_first_time) *cfg->out_first_time = false;

    if (!cfg || !cfg->key || !cfg->serialize) return BB_ERR_INVALID_ARG;
    if (strlen(cfg->key) >= BB_CACHE_KEY_MAX) {
        bb_log_e(TAG, "key '%s' too long (max %d chars)", cfg->key, BB_CACHE_KEY_MAX - 1);
        return BB_ERR_INVALID_ARG;
    }

    // AGE_OUT eviction validation (B1-592 A3). BB_CACHE_EVICT_PINNED (the
    // zero-init default) is unrestricted -- these three checks apply ONLY to
    // BB_CACHE_EVICT_AGE_OUT, so they nest under a single outer policy check
    // rather than each being a standalone compound `&&` condition (CI/GCC
    // branch coverage is the source of truth and splits compounds
    // unpredictably -- each rejection below is its own single-term `if`).
    if (cfg->eviction.policy == BB_CACHE_EVICT_AGE_OUT) {
        if (cfg->eviction.evict_age_ms == 0) {
            bb_log_e(TAG, "key '%s': AGE_OUT requires non-zero evict_age_ms", cfg->key);
            return BB_ERR_INVALID_ARG;
        }
        if (cfg->eviction.stale_age_ms >= cfg->eviction.evict_age_ms) {
            bb_log_e(TAG, "key '%s': stale_age_ms (%" PRIu32 ") must be < evict_age_ms (%" PRIu32 ")",
                     cfg->key, cfg->eviction.stale_age_ms, cfg->eviction.evict_age_ms);
            return BB_ERR_INVALID_ARG;
        }
        if (cfg->snapshot != NULL) {
            // Getter/refresh mode re-stamps ts_ms to now on every read, so
            // age-out is meaningless there -- AGE_OUT is only valid on OWNED
            // entries (snapshot == NULL, update-stamped ts_ms).
            bb_log_e(TAG, "key '%s': AGE_OUT is only valid on owned entries (snapshot must be NULL)", cfg->key);
            return BB_ERR_INVALID_ARG;
        }
    }

    ensure_init();

    pthread_mutex_lock(&s_reg_lock);

    // Idempotent: already registered? Find-or-init happens under this SINGLE
    // s_reg_lock acquisition (B1-592 firmware-review fix) -- callers that need
    // atomic first-time detection (cfg->out_first_time) must never pair a
    // separate bb_cache_exists() probe with this call, which would leave a
    // TOCTOU window between the two lock acquisitions.
    if (find_entry(cfg->key)) {
        pthread_mutex_unlock(&s_reg_lock);
        return BB_OK;
    }

    // Find a free slot
    bb_cache_entry_t *slot = NULL;
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] == '\0') {
            slot = &s_entries[i];
            break;
        }
    }

    if (!slot) {
        pthread_mutex_unlock(&s_reg_lock);
        bb_log_e(TAG, "registry full (max %d)", BB_CACHE_MAX_TOPICS);
        return BB_ERR_NO_SPACE;
    }

    // Tri-state ownership (see bb_cache.h): snap_size > 0 allocates an owned
    // buffer regardless of snapshot -- covers both plain OWNED (snapshot ==
    // NULL, size mandatory) and OWNED+FALLBACK (snapshot != NULL, size
    // opts in the cold-start seed). snap_size == 0 with snapshot == NULL is
    // invalid (owned mode needs a size); snap_size == 0 with snapshot !=
    // NULL is plain GETTER mode (no owned buffer).
    void *owned = NULL;
    if (cfg->snap_size > 0) {
        owned = bb_calloc_prefer_spiram(1, cfg->snap_size);
        if (!owned) {
            pthread_mutex_unlock(&s_reg_lock);
            return BB_ERR_NO_SPACE;
        }
    } else if (!cfg->snapshot) {
        pthread_mutex_unlock(&s_reg_lock);
        return BB_ERR_INVALID_ARG;
    }

    // slot->lock is process-lifetime (created once in ensure_init(), never
    // destroyed) -- do NOT touch it here. Every free-slot -> in-use
    // transition (fresh slot or a bb_cache_delete()'d one) bumps generation
    // so any reader holding a pre-transition (entry, generation) pair from
    // find_entry_locked_ref() detects the mismatch and bails with
    // BB_ERR_NOT_FOUND (see that function's doc comment). No reader can be
    // racing this write at this point because register() itself never
    // touches slot->lock and holds s_reg_lock across this entire function --
    // see find_entry_locked_ref()'s invariant.
    slot->generation++;

    bb_strlcpy(slot->key, cfg->key, sizeof(slot->key));
    slot->snapshot    = cfg->snapshot;
    slot->owned       = owned;
    slot->size        = cfg->snap_size;
    slot->has_value   = false;
    slot->fallback_seeded = false;
    slot->fn          = cfg->serialize;
    slot->flags       = cfg->flags;
    slot->cached_json = NULL;
    slot->cached_len  = 0;
    slot->dirty       = true;   // no bytes cached yet
    slot->ts_ms       = 0;      // stamped on first update()/snapshot() read
    slot->policy       = cfg->eviction.policy;
    slot->stale_age_ms = cfg->eviction.stale_age_ms;
    slot->evict_age_ms = cfg->eviction.evict_age_ms;
    slot->state_version = 0;  // B1-767 PR-3: fresh incarnation starts at 0

    pthread_mutex_unlock(&s_reg_lock);
    if (cfg->out_first_time) *cfg->out_first_time = true;
    return BB_OK;
}

bb_err_t bb_cache_update(const bb_cache_update_t *req)
{
    if (!req || !req->key || !req->snap) return BB_ERR_INVALID_ARG;

    ensure_init();

    entry_ref_t ref = find_entry_locked_ref(req->key);
    if (!ref.entry) return BB_ERR_NOT_FOUND;
    bb_cache_entry_t *e = ref.entry;

    BB_CACHE_TEST_RACE_POINT(req->key);
    pthread_mutex_lock(&e->lock);
    if (!entry_matches_locked(e, req->key, ref.generation)) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NOT_FOUND;
    }

    // Plain getter-mode (no owned buffer): caller owns the struct; update is
    // a no-op. No owned bytes to diff against, so changed is always reported
    // false. Gated on e->owned (not e->snapshot) so owned+fallback entries
    // (which have BOTH set) fall through to the real write path below.
    if (!e->owned) {
        pthread_mutex_unlock(&e->lock);
        if (req->out_changed) *req->out_changed = false;
        return BB_OK;
    }

    // Compute change BEFORE the copy-in: first write since register is always
    // a change (guards the false negative where memcmp against a
    // zero-initialized owned buffer would otherwise report unchanged).
    bool changed = (!e->has_value) || (memcmp(req->snap, e->owned, e->size) != 0);
    memcpy(e->owned, req->snap, e->size);
    e->has_value = true;
    // Envelope sample-time (owned mode): default to now; req->ts_ms overrides
    // when the caller supplies its own sample time (e.g. ingress/self-emit
    // source timestamp).
    e->ts_ms = req->ts_ms != 0 ? req->ts_ms : (int64_t)cache_now_ms64();
    e->dirty = true;   // invalidate memoized bytes; do NOT serialize here
    // state_version (B1-767 PR-3): bumps on EVERY successful owned write,
    // unconditionally -- even when `changed` is false (an identical
    // rewrite still counts as a write). Captured under the same lock as
    // the bump so the fired value always matches what a concurrent
    // bb_cache_state_version()/bb_cache_snapshot() would observe.
    e->state_version++;
    uint32_t v = e->state_version;
    pthread_mutex_unlock(&e->lock);

    if (req->out_changed) *req->out_changed = changed;

    // Write-notify (B1-767 PR-3): fired OUTSIDE the lock, on every
    // successful owned write, mirroring the evict-notify hook's
    // outside-locks contract so an installed hook may safely call back
    // into bb_cache without deadlocking.
    bb_cache_write_notify_fn nfy = atomic_load_explicit(&s_write_notify_fn, memory_order_relaxed);
    if (nfy) nfy(req->key, v);

    return BB_OK;
}

// Shared teardown for an ALREADY-FOUND entry (firmware-review fix, revises
// A3). Caller holds s_reg_lock across this call (both bb_cache_delete() and
// bb_cache_delete_if_generation() hold it across their entire find+teardown,
// for the same "no concurrent register/delete can observe a half-torn-down
// slot" reason documented on bb_cache_delete() below). Frees the entry's
// owned/cached_json buffers, bumps generation (tombstone -- see
// find_entry_locked_ref()'s doc comment), and marks the slot free for reuse.
// Does NOT release s_reg_lock or fire the evict-notify hook -- both callers
// do that themselves immediately after this returns.
static void teardown_found_entry_locked(bb_cache_entry_t *e)
{
    pthread_mutex_lock(&e->lock);
    free_entry_locked(e);
    e->generation++;
    pthread_mutex_unlock(&e->lock);

    e->key[0] = '\0';  // last: marks the slot free for bb_cache_register reuse
}

bb_err_t bb_cache_delete(const char *key)
{
    if (!key) return BB_ERR_INVALID_ARG;

    ensure_init();

    // Deliberate exception to the find_entry_locked_ref pattern: this function
    // holds s_reg_lock across the ENTIRE delete (find + teardown + generation
    // bump + slot free), not just the lookup. A narrower critical section
    // (lock, find, unlock, then lock again to tear down) would open a window
    // where a concurrent bb_cache_register() observes the about-to-be-deleted
    // slot as still "in use" (key[0] != '\0') and picks a DIFFERENT free
    // slot, or — worse — a concurrent delete of the same key races this one.
    // Holding s_reg_lock for the whole operation makes the delete atomic with
    // respect to every other registry-mutating call (register/delete),
    // matching how bb_cache_register already holds s_reg_lock across its own
    // find+init.
    //
    // String-keyed by design: this is the PUBLIC, app-facing delete (a
    // "graceful bye" for a key the app knows by name) -- the app has no
    // generation to hand back, so it deletes whatever incarnation is
    // CURRENTLY registered under `key`. The identity-keyed (generation-
    // checked) variant used internally by age-out eviction is
    // bb_cache_delete_if_generation() below.
    pthread_mutex_lock(&s_reg_lock);

    bb_cache_entry_t *e = find_entry(key);
    if (!e) {
        pthread_mutex_unlock(&s_reg_lock);
        return BB_ERR_NOT_FOUND;
    }

    teardown_found_entry_locked(e);

    pthread_mutex_unlock(&s_reg_lock);

    // Evict-notify hook (B1-592 A3, revises A2): fired for EVERY successful
    // delete -- explicit callers of bb_cache_delete() AND the LAZY/SWEEP
    // age-out eviction paths (which call bb_cache_delete_if_generation()
    // internally, see evict_if_aged_out_locked()) -- outside all bb_cache
    // locks, so the installed hook (currently bb_mdns_cache's on_peer_evicted,
    // B1-1118) may safely call back into bb_cache without deadlocking.
    void (*notify)(const char *) = atomic_load_explicit(&s_evict_notify_fn, memory_order_relaxed);
    if (notify) notify(key);
    return BB_OK;
}

// Internal, identity-keyed delete (firmware-review fix, revises A3): used
// ONLY by the age-out eviction paths (evict_if_aged_out_locked(), shared by
// the LAZY read-time floor and the SWEEP backstop's sweep_cb()). Deletes and
// fires the evict-notify hook ONLY if the entry CURRENTLY registered under
// `key` still has generation == expected_gen -- i.e. it is provably the exact
// incarnation the caller observed as aged-out, not a brand-new registration
// of the same key that landed in the window between the caller releasing
// e->lock and this call running (two evictors racing each other, or an
// evictor racing a same-key bb_cache_register() reuse). On a mismatch (or the
// key no longer existing at all), this is a silent no-op: the found-later
// incarnation is a DIFFERENT entry that this eviction pass has no business
// touching.
//
// Not app-facing (no bb_cache.h declaration) -- see bb_cache_delete()'s doc
// comment for why the public, string-keyed delete has different semantics.
static bb_err_t bb_cache_delete_if_generation(const char *key, uint32_t expected_gen)
{
    ensure_init();

    // Same whole-operation s_reg_lock rationale as bb_cache_delete() above.
    pthread_mutex_lock(&s_reg_lock);

    bb_cache_entry_t *e = find_entry(key);
    if (!e) {
        pthread_mutex_unlock(&s_reg_lock);
        return BB_ERR_NOT_FOUND;
    }
    if (e->generation != expected_gen) {
        // A different incarnation (deleted-and-reused, or freshly
        // re-registered) now occupies this key -- not the one this eviction
        // pass observed as aged-out. Leave it alone.
        pthread_mutex_unlock(&s_reg_lock);
        return BB_OK;
    }

    teardown_found_entry_locked(e);

    pthread_mutex_unlock(&s_reg_lock);

    void (*notify)(const char *) = atomic_load_explicit(&s_evict_notify_fn, memory_order_relaxed);
    if (notify) notify(key);
    return BB_OK;
}

bool bb_cache_exists(const char *key)
{
    if (!key) return false;

    ensure_init();

    return find_entry_locked(key) != NULL;
}

bb_err_t bb_cache_serialize_into(const char *key, bb_json_t obj)
{
    if (!key || !obj) return BB_ERR_INVALID_ARG;

    ensure_init();

    entry_ref_t ref = find_entry_locked_ref(key);
    if (!ref.entry) return BB_ERR_NOT_FOUND;

    // No envelope here by design — this call embeds the key's fields directly
    // as a section of a larger composed document (see header comment).
    return serialize_locked(ref.entry, key, ref.generation, obj, NULL);
}

bb_err_t bb_cache_get_serialized(const char *key, char *buf, size_t cap, size_t *out_len)
{
    if (!key || !buf || cap == 0) return BB_ERR_INVALID_ARG;

    ensure_init();

    entry_ref_t ref = find_entry_locked_ref(key);
    if (!ref.entry) return BB_ERR_NOT_FOUND;
    bb_cache_entry_t *e = ref.entry;

    BB_CACHE_TEST_RACE_POINT(key);
    pthread_mutex_lock(&e->lock);
    if (!entry_matches_locked(e, key, ref.generation)) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NOT_FOUND;
    }
    if (evict_if_aged_out_locked(e)) {
        return BB_ERR_NOT_FOUND;
    }

    // Plain getter-mode entries (no owned buffer) have no dirty signal (data
    // can change without an update), so always re-serialize. Owned-mode and
    // owned+fallback entries (both have an owned buffer) memoize via dirty.
    //
    // e->cached_json == NULL is provably unreachable whenever e->dirty is
    // false: the ONLY site that sets dirty = false is immediately after
    // cached_json = s (a non-NULL successful bb_json_serialize result) a few
    // lines below; every other write to dirty sets it true, and every write
    // to cached_json = NULL (free_entry_locked, bb_cache_register) is always
    // paired with dirty = true in the same scope. So "dirty false, cached_json
    // NULL" can never coexist -- LCOV_EXCL_BR_LINE on the middle term below.
    bool need = e->dirty || e->cached_json == NULL || is_plain_getter(e);  // LCOV_EXCL_BR_LINE
    if (need) {
        const void *snap;
        if (is_plain_getter(e)) {
            snap = e->snapshot();
            e->ts_ms = (int64_t)cache_now_ms64();  // getter mode: read IS the sample
        } else {
            maybe_seed_fallback(e);  // owned+fallback: seed if still unpopulated
            snap = e->owned;
        }
        if (!snap) {
            pthread_mutex_unlock(&e->lock);
            bb_log_w(TAG, "key '%s': no snapshot available", e->key);
            return BB_ERR_INVALID_STATE;
        }

        bb_json_t obj = bb_json_obj_new();
        if (!obj) {
            pthread_mutex_unlock(&e->lock);
            return BB_ERR_NO_SPACE;
        }

        // The serializer runs exactly once per generation here. cached_json
        // holds only the inner "data" bytes -- the envelope ({"ts_ms":N,
        // "data":...}) is applied below, around the memoized string, on every
        // read (owned mode: ts_ms is frozen between updates, so the envelope
        // bytes stay byte-identical across reads within an interval).
        e->fn(obj, snap);
        char *s = bb_json_serialize(obj);
        bb_json_free(obj);
        if (!s) {
            pthread_mutex_unlock(&e->lock);
            return BB_ERR_NO_SPACE;
        }

        if (e->cached_json) bb_json_free_str(e->cached_json);
        e->cached_json = s;
        e->cached_len  = strlen(s);
        e->dirty       = false;
    }

    // Wrap the memoized "data" bytes in the envelope and copy out under the
    // lock. Compute the required length first (snprintf(NULL,0,...)) so an
    // undersized buffer is refused WITHOUT a partial write, matching the
    // pre-envelope contract ("buf untouched" on BB_ERR_NO_SPACE).
    int need_len = snprintf(NULL, 0, "{\"ts_ms\":%" PRId64 ",\"data\":%s}",
                             e->ts_ms, e->cached_json);
    if (need_len < 0) {  // LCOV_EXCL_BR_LINE -- snprintf only returns negative
                          // on an encoding error; unreachable with the fixed,
                          // well-formed format string above, and there is no
                          // host fault-injection seam for libc's snprintf.
        // LCOV_EXCL_START -- body of the unreachable branch above; see the
        // LCOV_EXCL_BR_LINE justification on the `if` immediately above.
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NO_SPACE;
        // LCOV_EXCL_STOP
    }
    if ((size_t)need_len + 1 > cap) {
        pthread_mutex_unlock(&e->lock);
        bb_log_w(TAG, "key '%s': buffer too small (need %d, cap %zu)",
                 key, need_len + 1, cap);
        return BB_ERR_NO_SPACE;
    }
    snprintf(buf, cap, "{\"ts_ms\":%" PRId64 ",\"data\":%s}", e->ts_ms, e->cached_json);
    if (out_len) *out_len = (size_t)need_len;

    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Keyed enumeration + compact struct-read accessor
// ---------------------------------------------------------------------------

size_t bb_cache_count(void)
{
    ensure_init();

    size_t count = 0;
    pthread_mutex_lock(&s_reg_lock);
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] != '\0') count++;
    }
    pthread_mutex_unlock(&s_reg_lock);
    return count;
}

bb_err_t bb_cache_key_at(size_t index, char *buf, size_t cap)
{
    if (!buf || cap == 0) return BB_ERR_INVALID_ARG;
    if (index >= (size_t)BB_CACHE_MAX_TOPICS) return BB_ERR_NOT_FOUND;

    ensure_init();

    // Copy the key BY VALUE under s_reg_lock -- never hand back a raw pointer
    // into s_entries[] (same UAF class the bb_cache_foreach fix eliminated:
    // a concurrent delete+re-register could rename the slot's key bytes out
    // from under a caller still holding a raw pointer).
    pthread_mutex_lock(&s_reg_lock);
    size_t len = strlen(s_entries[index].key);
    if (len + 1 > cap) {
        pthread_mutex_unlock(&s_reg_lock);
        return BB_ERR_NO_SPACE;
    }
    memcpy(buf, s_entries[index].key, len + 1);
    pthread_mutex_unlock(&s_reg_lock);
    return BB_OK;
}

bb_err_t bb_cache_foreach(void (*cb)(const char *key, void *ctx), void *ctx)
{
    if (!cb) return BB_ERR_INVALID_ARG;

    ensure_init();

    // Snapshot keys BY VALUE (memcpy the bytes) under the registry lock, then
    // release before invoking cb — avoids reentrancy deadlock if cb calls
    // back into bb_cache. Keys are no longer add-only (bb_cache_delete can
    // free and reuse a slot), so a snapshot of raw pointers into s_entries[]
    // would risk a concurrent delete+re-register renaming the slot's key
    // bytes under an in-flight cb; copying the bytes onto this function's
    // stack sidesteps that entirely. Stack cost:
    // BB_CACHE_MAX_TOPICS * BB_CACHE_KEY_MAX (default 32*96 = 3 KB).
    char keys[BB_CACHE_MAX_TOPICS][BB_CACHE_KEY_MAX];
    int n = 0;

    pthread_mutex_lock(&s_reg_lock);
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] != '\0') {
            memcpy(keys[n], s_entries[i].key, BB_CACHE_KEY_MAX);
            n++;
        }
    }
    pthread_mutex_unlock(&s_reg_lock);

    for (int i = 0; i < n; i++) {
        cb(keys[i], ctx);
    }
    return BB_OK;
}

bb_err_t bb_cache_get_raw(const char *key, void *buf, size_t cap)
{
    if (!key || !buf || cap == 0) return BB_ERR_INVALID_ARG;

    ensure_init();

    entry_ref_t ref = find_entry_locked_ref(key);
    if (!ref.entry) return BB_ERR_NOT_FOUND;
    bb_cache_entry_t *e = ref.entry;

    BB_CACHE_TEST_RACE_POINT(key);
    pthread_mutex_lock(&e->lock);
    if (!entry_matches_locked(e, key, ref.generation)) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NOT_FOUND;
    }
    if (evict_if_aged_out_locked(e)) {
        return BB_ERR_NOT_FOUND;
    }
    return copy_owned_and_capture_locked(e, "get_raw", key, buf, cap, NULL, NULL);
}

// ---------------------------------------------------------------------------
// state_version + walk-safe snapshot (B1-767 PR-3)
// ---------------------------------------------------------------------------

bb_err_t bb_cache_snapshot(const char *key, void *buf, size_t cap, bb_cache_snapshot_t *out)
{
    if (!key || !buf || !out) return BB_ERR_INVALID_ARG;

    ensure_init();

    entry_ref_t ref = find_entry_locked_ref(key);
    if (!ref.entry) return BB_ERR_NOT_FOUND;
    bb_cache_entry_t *e = ref.entry;

    BB_CACHE_TEST_RACE_POINT(key);
    pthread_mutex_lock(&e->lock);
    if (!entry_matches_locked(e, key, ref.generation)) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NOT_FOUND;
    }
    if (evict_if_aged_out_locked(e)) {
        return BB_ERR_NOT_FOUND;
    }
    bb_err_t err = copy_owned_and_capture_locked(e, "snapshot", key, buf, cap,
                                                  &out->size, &out->version);
    if (err != BB_OK) return err;
    out->state = buf;  // safe post-unlock: caller's own buffer pointer, not e-owned
    return BB_OK;
}

bb_err_t bb_cache_state_version(const char *key, uint32_t *out_version)
{
    if (!key || !out_version) return BB_ERR_INVALID_ARG;

    ensure_init();

    entry_ref_t ref = find_entry_locked_ref(key);
    if (!ref.entry) return BB_ERR_NOT_FOUND;
    bb_cache_entry_t *e = ref.entry;

    BB_CACHE_TEST_RACE_POINT(key);
    pthread_mutex_lock(&e->lock);
    if (!entry_matches_locked(e, key, ref.generation)) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NOT_FOUND;
    }
    // Non-destructive -- no evict_if_aged_out_locked() here, matching
    // bb_cache_is_stale()'s contract (a probe, not a read call site).
    *out_version = e->state_version;
    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Age-out eviction (B1-592 A3)
// ---------------------------------------------------------------------------

bb_err_t bb_cache_is_stale(const char *key, bool *out_stale)
{
    if (!key) return BB_ERR_INVALID_ARG;
    if (!out_stale) return BB_ERR_INVALID_ARG;

    ensure_init();

    entry_ref_t ref = find_entry_locked_ref(key);
    if (!ref.entry) return BB_ERR_NOT_FOUND;
    bb_cache_entry_t *e = ref.entry;

    BB_CACHE_TEST_RACE_POINT(key);
    pthread_mutex_lock(&e->lock);
    if (!entry_matches_locked(e, key, ref.generation)) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NOT_FOUND;
    }

    // Deliberately does NOT evict here -- this is a non-destructive staleness
    // probe (see bb_cache.h doc comment), not a read call site. PINNED-policy
    // entries and entries with no stale window (stale_age_ms == 0) always
    // report false while present.
    bool stale = false;
    if (e->policy == BB_CACHE_EVICT_AGE_OUT) {
        uint64_t age = cache_now_ms64() - (uint64_t)e->ts_ms;
        stale = bb_cache_evaluate_age(age, e->stale_age_ms, e->evict_age_ms) != BB_CACHE_ENTRY_FRESH;
    }
    pthread_mutex_unlock(&e->lock);

    *out_stale = stale;
    return BB_OK;
}

// SWEEP backstop callback (B1-592 A3): invoked once per registered key by
// bb_cache_foreach()'s snapshot-then-notify shape (s_reg_lock is NOT held
// here). Re-finds the key fresh (find_entry_locked_ref) rather than trusting
// any pointer captured before foreach's snapshot -- the entry may have been
// deleted, or the slot reused for a different key, by the time this callback
// runs. Reuses evict_if_aged_out_locked() -- the same evaluate+evict-under-
// e->lock-then-release-then-delete logic as the LAZY read-time floor -- so
// SWEEP is edge-triggered naturally: a freed slot is simply absent from the
// NEXT bb_cache_foreach() snapshot, no separate bookkeeping needed.
//
// Only compiled when something can actually call it (the real periodic
// driver below, gated on BB_CACHE_SWEEP_ENABLE, or the host-test direct
// invocation, gated on BB_CACHE_TESTING) -- otherwise an unused static
// function would warn on a production build with the sweep Kconfig off.
#if BB_CACHE_SWEEP_ENABLE || defined(BB_CACHE_TESTING)
static void sweep_cb(const char *key, void *ctx)
{
    (void)ctx;

    entry_ref_t ref = find_entry_locked_ref(key);
    if (!ref.entry) return;
    bb_cache_entry_t *e = ref.entry;

    BB_CACHE_TEST_RACE_POINT(key);
    pthread_mutex_lock(&e->lock);
    if (!entry_matches_locked(e, key, ref.generation)) {
        pthread_mutex_unlock(&e->lock);
        return;
    }
    if (evict_if_aged_out_locked(e)) {
        return;  // e->lock already released by evict_if_aged_out_locked
    }
    pthread_mutex_unlock(&e->lock);
}
#endif // BB_CACHE_SWEEP_ENABLE || BB_CACHE_TESTING

#if BB_CACHE_SWEEP_ENABLE

// Worker task stack in bytes. Tunable via CONFIG_BB_CACHE_SWEEP_WORKER_STACK
// (Kconfig, default 8192). Sized generously for a TLS-capable worker stack,
// because sweep_cb()'s call chain is consumer-controlled at its far end:
// sweep_cb -> evict_if_aged_out_locked
// -> bb_cache_delete_if_generation -> the evict-notify hook -> (when a
// composer has installed one, e.g. bb_mdns_cache) an ARBITRARY
// consumer-supplied on_remove callback. bb_cache has no way to bound that
// last hop's stack depth, so it cannot safely run on the ESP-IDF pthread
// default (~3072 bytes) the sweep used before this firmware-review fix.
#ifndef BB_CACHE_SWEEP_WORKER_STACK
#  if defined(CONFIG_BB_CACHE_SWEEP_WORKER_STACK)
#    define BB_CACHE_SWEEP_WORKER_STACK CONFIG_BB_CACHE_SWEEP_WORKER_STACK
#  else
#    define BB_CACHE_SWEEP_WORKER_STACK 8192
#  endif
#endif

// Worker task priority. Tunable via CONFIG_BB_CACHE_SWEEP_WORKER_PRIORITY
// (Kconfig). Default 1 (lowest app priority).
#ifndef BB_CACHE_SWEEP_WORKER_PRIORITY
#  if defined(CONFIG_BB_CACHE_SWEEP_WORKER_PRIORITY)
#    define BB_CACHE_SWEEP_WORKER_PRIORITY CONFIG_BB_CACHE_SWEEP_WORKER_PRIORITY
#  else
#    define BB_CACHE_SWEEP_WORKER_PRIORITY 1
#  endif
#endif

// Worker task core affinity. Tunable via CONFIG_BB_CACHE_SWEEP_WORKER_CORE
// (Kconfig). Default -1 (tskNO_AFFINITY).
#ifndef BB_CACHE_SWEEP_WORKER_CORE
#  if defined(CONFIG_BB_CACHE_SWEEP_WORKER_CORE)
#    define BB_CACHE_SWEEP_WORKER_CORE CONFIG_BB_CACHE_SWEEP_WORKER_CORE
#  else
#    define BB_CACHE_SWEEP_WORKER_CORE (-1)
#  endif
#endif

static bb_periodic_timer_t s_sweep_timer = NULL;

static void sweep_work_fn(void *arg)
{
    (void)arg;
    bb_cache_foreach(sweep_cb, NULL);
}

// pre_http registry hook (B1-592 lifecycle follow-up, firmware-review fix:
// replaces a raw pthread_create() + manual usleep() loop with the house
// bb_timer_worker pattern: a dedicated worker task
// created via bb_timer_worker_periodic_create, sized/named/prioritized
// explicitly instead of inheriting the ESP-IDF pthread default. This file is
// shared verbatim with the host build (see platform/host/bb_cache/
// bb_cache_host.c); BB_CACHE_SWEEP_ENABLE is only ever nonzero under
// ESP_PLATFORM (host never defines the CONFIG_BB_CACHE_SWEEP_ENABLE
// Kconfig), so the outer BB_CACHE_SWEEP_ENABLE guard alone is sufficient --
// host never links bb_timer and never sees this block.
//
// The Kconfig knob (CONFIG_BB_CACHE_SWEEP_ENABLE) is the ONLY control: it
// gates whether this fn (and its bbtool:init marker in bb_cache.h) even
// compiles in -- see bb_cache.h.
//
// Idempotent via s_sweep_timer: NULL means "not started" -- on a failed
// create, s_sweep_timer is left/reset to NULL rather than being set true
// before the call succeeds (the firmware-review-flagged lying-state bug),
// so a create failure is loudly logged and never silently+permanently
// disables the sweep behind a state that claims otherwise. The existing
// bb_cache_evict_sweep_once_for_test() (BB_CACHE_TESTING) bypasses this
// worker entirely and calls sweep_cb's driver (bb_cache_foreach(sweep_cb,
// NULL)) directly -- unaffected by this change.
//
// Host-test seam limitation: this entire block (including this function)
// only compiles when BB_CACHE_SWEEP_ENABLE is nonzero, which requires
// ESP_PLATFORM + CONFIG_BB_CACHE_SWEEP_ENABLE=y -- it is never compiled into
// the host test binary at all (BB_CACHE_SWEEP_ENABLE is hard-0 off
// ESP_PLATFORM, see bb_cache.h). There is therefore no host-reachable seam
// to fault-inject bb_timer_worker_periodic_create()'s failure branch through
// a BB_CACHE_TESTING hook -- unlike e.g. bb_alloc_inject seams elsewhere,
// contorting this component to expose one would mean linking bb_timer
// into the host build for a code path host never runs. The
// create-failure branch is covered by inspection only; the esp32-cache-sweep
// smoke build is the live-link proof that this code path compiles and links
// correctly on-device.
bb_err_t bb_cache_evict_start(void)
{
    if (s_sweep_timer) return BB_OK;  // already started (idempotent)

    ensure_init();

    bb_timer_worker_cfg_t cfg = {
        .stack    = BB_CACHE_SWEEP_WORKER_STACK,
        .priority = BB_CACHE_SWEEP_WORKER_PRIORITY,
        .core     = BB_CACHE_SWEEP_WORKER_CORE,
    };
    bb_err_t err = bb_timer_worker_periodic_create(sweep_work_fn, NULL, "bb_cache_evict",
                                                    &cfg, &s_sweep_timer);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to create eviction sweep worker: %d", err);
        s_sweep_timer = NULL;  // belt-and-suspenders: never leave a lying started-state
        return err;
    }

    err = bb_timer_periodic_start(s_sweep_timer, (uint64_t)BB_CACHE_SWEEP_PERIOD_MS * 1000ULL);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to start eviction sweep timer: %d", err);
        bb_timer_periodic_delete(s_sweep_timer);
        s_sweep_timer = NULL;  // retry-able on a future call, not a lying "started" state
        return err;
    }

    bb_log_i(TAG, "eviction sweep started; period=%d ms", BB_CACHE_SWEEP_PERIOD_MS);
    return BB_OK;
}
#endif // BB_CACHE_SWEEP_ENABLE

// ---------------------------------------------------------------------------
// Test reset (guarded by BB_CACHE_TESTING)
// ---------------------------------------------------------------------------

#ifdef BB_CACHE_TESTING
void bb_cache_reset_for_test(void)
{
    pthread_mutex_lock(&s_reg_lock);
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        if (s_entries[i].key[0] != '\0') {
            // Reuse free_entry_locked for the owned/cached_json/dirty/
            // has_value/fallback_seeded teardown (dedup -- was hand-rolled
            // here, duplicating bb_cache_delete()'s logic). Deliberately does
            // NOT call pthread_mutex_destroy: the mutex is process-lifetime
            // (see ensure_init()'s comment) and is NEVER destroyed, including
            // in test teardown. s_init_once (pthread_once) is likewise never
            // reset, so a later ensure_init() call in the SAME test binary
            // never attempts to re-pthread_mutex_init an already-valid mutex
            // (UB per POSIX).
            pthread_mutex_lock(&s_entries[i].lock);
            free_entry_locked(&s_entries[i]);
            pthread_mutex_unlock(&s_entries[i].lock);

            s_entries[i].key[0]      = '\0';
            s_entries[i].snapshot    = NULL;
            s_entries[i].fn          = NULL;
            s_entries[i].flags       = BB_CACHE_FLAG_NONE;
            s_entries[i].ts_ms       = 0;
            s_entries[i].generation  = 0;
            s_entries[i].policy       = BB_CACHE_EVICT_PINNED;
            s_entries[i].stale_age_ms = 0;
            s_entries[i].evict_age_ms = 0;
        }
    }
    pthread_mutex_unlock(&s_reg_lock);
    // Test isolation: a prior test's installed evict-notify hook, write-
    // notify hook, or fake clock must never leak into the next test.
    atomic_store_explicit(&s_evict_notify_fn, NULL, memory_order_relaxed);
    atomic_store_explicit(&s_write_notify_fn, NULL, memory_order_relaxed);
    s_test_clock_hook = NULL;
}

// Test-only direct invocation of the SWEEP backstop's single pass, bypassing
// the Kconfig-gated periodic driver (bb_cache_evict_start()) entirely so
// tests can exercise sweep_cb() deterministically with no real threads or
// sleeps -- pairs with bb_cache_test_set_clock() (see test_bb_cache_evict.c).
void bb_cache_evict_sweep_once_for_test(void)
{
    ensure_init();
    bb_cache_foreach(sweep_cb, NULL);
}
#endif
