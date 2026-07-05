// Must come before any system header on Linux — glibc gates PTHREAD_MUTEX_RECURSIVE
// on _GNU_SOURCE (or _XOPEN_SOURCE >= 500). macOS exposes it unconditionally.
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "bb_cache.h"
#include "bb_event.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_core.h"
#include "bb_mem.h"
#include "bb_clock.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

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
    bb_event_topic_t     event_topic;     // registered event topic handle (NULL if no SSE)
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
    // Tombstone/generation guard (B1-592 firmware-review fix): bumped every
    // time this slot transitions free -> in-use (bb_cache_register populating
    // a freed or never-used slot) AND every time it transitions in-use ->
    // free (bb_cache_delete). A reader that captures (entry pointer,
    // generation) under s_reg_lock and re-validates BOTH key and generation
    // after acquiring e->lock is guaranteed to either observe the exact
    // incarnation it looked up, or detect the mismatch and bail with
    // BB_ERR_NOT_FOUND -- see find_entry_locked_ref()'s doc comment.
    uint32_t             generation;
} bb_cache_entry_t;

static bb_cache_entry_t s_entries[BB_CACHE_MAX_TOPICS];
static pthread_mutex_t  s_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t   s_init_once = PTHREAD_ONCE_INIT;

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
// per-entry teardown EXACTLY. Caller holds e->lock. Does NOT touch e->key, e->event_topic,
// e->generation, or the mutex itself -- those are the caller's responsibility
// (bb_cache_delete bumps generation and clears key/event_topic itself, from
// its own scope, after this helper returns).
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
#else
#define BB_CACHE_TEST_RACE_POINT(key) ((void)0)
#endif

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
        e->ts_ms = (int64_t)bb_clock_now_ms64();  // seed IS a sample
    }
    e->fallback_seeded = true;  // never retry, even if snapshot() returned NULL
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
// out_topic, when non-NULL, receives e->event_topic -- read under the same
// lock+validation so callers (bb_cache_post) never read event_topic unguarded.
static bb_err_t serialize_locked(bb_cache_entry_t *e, const char *key, uint32_t generation,
                                  bb_json_t obj, int64_t *out_ts_ms, bb_event_topic_t *out_topic)
{
    BB_CACHE_TEST_RACE_POINT(key);
    pthread_mutex_lock(&e->lock);

    if (!entry_matches_locked(e, key, generation)) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_NOT_FOUND;
    }

    if (out_topic) *out_topic = e->event_topic;

    const void *snap;
    if (is_plain_getter(e)) {
        // Plain getter mode: no owned buffer, always pull through.
        snap = e->snapshot();
        e->ts_ms = (int64_t)bb_clock_now_ms64();
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
    if (!cfg || !cfg->key || !cfg->serialize) return BB_ERR_INVALID_ARG;
    if (strlen(cfg->key) >= BB_CACHE_KEY_MAX) {
        bb_log_e(TAG, "key '%s' too long (max %d chars)", cfg->key, BB_CACHE_KEY_MAX - 1);
        return BB_ERR_INVALID_ARG;
    }

    ensure_init();

    pthread_mutex_lock(&s_reg_lock);

    // Idempotent: already registered?
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

    // Register event topic only when SSE flag is set.
    // Sink-only entries (no SSE delivery) skip this — bb_cache_post returns
    // BB_ERR_INVALID_STATE when event_topic is NULL, guarding against misuse.
    bb_event_topic_t ev_topic = NULL;
    if (cfg->flags & BB_CACHE_FLAG_SSE) {
        bb_event_topic_register(cfg->key, &ev_topic);
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

    strncpy(slot->key, cfg->key, sizeof(slot->key) - 1);
    slot->key[sizeof(slot->key) - 1] = '\0';
    slot->snapshot    = cfg->snapshot;
    slot->owned       = owned;
    slot->size        = cfg->snap_size;
    slot->has_value   = false;
    slot->fallback_seeded = false;
    slot->fn          = cfg->serialize;
    slot->event_topic = ev_topic;
    slot->flags       = cfg->flags;
    slot->cached_json = NULL;
    slot->cached_len  = 0;
    slot->dirty       = true;   // no bytes cached yet
    slot->ts_ms       = 0;      // stamped on first update()/snapshot() read

    pthread_mutex_unlock(&s_reg_lock);
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
    e->ts_ms = req->ts_ms != 0 ? req->ts_ms : (int64_t)bb_clock_now_ms64();
    e->dirty = true;   // invalidate memoized bytes; do NOT serialize here
    pthread_mutex_unlock(&e->lock);

    if (req->out_changed) *req->out_changed = changed;
    return BB_OK;
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
    pthread_mutex_lock(&s_reg_lock);

    bb_cache_entry_t *e = find_entry(key);
    if (!e) {
        pthread_mutex_unlock(&s_reg_lock);
        return BB_ERR_NOT_FOUND;
    }

    pthread_mutex_lock(&e->lock);
    free_entry_locked(e);
    // Tombstone: bump generation so any reader holding a pre-delete (entry,
    // generation) pair from find_entry_locked_ref() detects the mismatch
    // after acquiring e->lock and bails with BB_ERR_NOT_FOUND, instead of
    // observing a torn-down or (once reused) a completely different key's
    // data under this lock. e->lock itself is NEVER destroyed here -- see
    // ensure_init()'s mutex-lifetime comment -- which is what makes this
    // guard sufficient without a destroyed-mutex UB risk.
    e->generation++;
    pthread_mutex_unlock(&e->lock);

    // Known Phase-A limitation: e->event_topic (if any, BB_CACHE_FLAG_SSE) is
    // intentionally NOT torn down here -- bb_event has no topic-unregister
    // primitive. See the loud warning on bb_cache_delete() in bb_cache.h.
    e->event_topic = NULL;

    e->key[0] = '\0';  // last: marks the slot free for bb_cache_register reuse

    pthread_mutex_unlock(&s_reg_lock);
    return BB_OK;
}

bool bb_cache_exists(const char *key)
{
    if (!key) return false;

    ensure_init();

    return find_entry_locked(key) != NULL;
}

bb_err_t bb_cache_post(const char *key)
{
    if (!key) return BB_ERR_INVALID_ARG;

    ensure_init();

    entry_ref_t ref = find_entry_locked_ref(key);
    if (!ref.entry) return BB_ERR_NOT_FOUND;
    bb_cache_entry_t *e = ref.entry;

    // Envelope (B1-570 PR-3): {"ts_ms":N,"data":{...}}. Serialize the
    // producer's fields into a nested "data" object rather than round-tripping
    // through a string, then attach ts_ms + data to the envelope root.
    bb_json_t data = bb_json_obj_new();
    if (!data) return BB_ERR_NO_SPACE;

    // serialize_locked re-validates (key, generation) under e->lock and also
    // hands back e->event_topic read under that SAME lock -- bb_cache_post
    // must never read e->event_topic unguarded (it can change identity the
    // instant a concurrent delete+re-register races this call).
    int64_t ts_ms = 0;
    bb_event_topic_t topic = NULL;
    bb_err_t err = serialize_locked(e, key, ref.generation, data, &ts_ms, &topic);
    if (err != BB_OK) {
        bb_json_free(data);
        return err;
    }
    if (!topic) {
        bb_json_free(data);
        return BB_ERR_INVALID_STATE;
    }

    bb_json_t root = bb_json_obj_new();
    if (!root) {
        bb_json_free(data);
        return BB_ERR_NO_SPACE;
    }
    bb_json_obj_set_int(root, "ts_ms", ts_ms);
    bb_json_obj_set_obj(root, "data", data);  // ownership of data transfers to root

    char *payload = bb_json_serialize(root);
    bb_json_free(root);
    if (!payload) return BB_ERR_NO_SPACE;

    size_t len = strlen(payload);
    err = bb_event_post(topic, 0, payload, len + 1);
    bb_json_free_str(payload);
    return err;
}

bb_err_t bb_cache_serialize_into(const char *key, bb_json_t obj)
{
    if (!key || !obj) return BB_ERR_INVALID_ARG;

    ensure_init();

    entry_ref_t ref = find_entry_locked_ref(key);
    if (!ref.entry) return BB_ERR_NOT_FOUND;

    // No envelope here by design — this call embeds the key's fields directly
    // as a section of a larger composed document (see header comment).
    return serialize_locked(ref.entry, key, ref.generation, obj, NULL, NULL);
}

bb_err_t bb_cache_post_serialized(const char *key, const char *json, size_t json_len)
{
    if (!key || !json) return BB_ERR_INVALID_ARG;

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
    // Read event_topic under the SAME lock+validation, never unguarded.
    bb_event_topic_t topic = e->event_topic;
    pthread_mutex_unlock(&e->lock);

    if (!topic) return BB_ERR_INVALID_STATE;

    return bb_event_post(topic, 0, json, json_len + 1);
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
            e->ts_ms = (int64_t)bb_clock_now_ms64();  // getter mode: read IS the sample
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
    if (!e->owned) {
        pthread_mutex_unlock(&e->lock);
        return BB_ERR_INVALID_STATE;
    }
    if (cap < e->size) {
        pthread_mutex_unlock(&e->lock);
        bb_log_w(TAG, "get_raw '%s': buf too small (%zu < %zu)", key, cap, e->size);
        return BB_ERR_NO_SPACE;
    }
    maybe_seed_fallback(e);  // owned+fallback: seed if still unpopulated
    memcpy(buf, e->owned, e->size);
    pthread_mutex_unlock(&e->lock);
    return BB_OK;
}

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
            s_entries[i].event_topic = NULL;
            s_entries[i].flags       = BB_CACHE_FLAG_NONE;
            s_entries[i].ts_ms       = 0;
            s_entries[i].generation  = 0;
        }
    }
    pthread_mutex_unlock(&s_reg_lock);
}
#endif
