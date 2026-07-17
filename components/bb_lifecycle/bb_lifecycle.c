// bb_lifecycle — service run-state authority. See include/bb_lifecycle.h for
// the full public contract; this file is the sole implementation (no
// platform/ split -- pure BSS state machine, no platform calls anywhere).
#include "bb_lifecycle.h"
#include "bb_lifecycle_priv.h"
#include "bb_callback_slot.h"
#include "bb_lock.h"
#include "bb_once.h"
#include "bb_log.h"

#include <stdatomic.h>
#include <string.h>

static const char *TAG = "bb_lifecycle";

// ---------------------------------------------------------------------------
// Kconfig -> C-default bridges. CONFIG_BB_LIFECYCLE_MAX_REASONS is already
// bridged in bb_lifecycle.h (needed there for the BB_LIFECYCLE_INHIBIT_WORDS
// math); the rest are bridged here. Never #undef/shadow a generated
// CONFIG_ symbol -- only fill in a C default when it is absent.
// ---------------------------------------------------------------------------
#ifndef CONFIG_BB_LIFECYCLE_MAX_SERVICES
#define CONFIG_BB_LIFECYCLE_MAX_SERVICES 8
#endif
#ifndef CONFIG_BB_LIFECYCLE_MAX_OBSERVERS
#define CONFIG_BB_LIFECYCLE_MAX_OBSERVERS 8
#endif
#ifndef CONFIG_BB_LIFECYCLE_NAME_MAX
#define CONFIG_BB_LIFECYCLE_NAME_MAX 24
#endif
#ifndef CONFIG_BB_LIFECYCLE_REASON_MAX
#define CONFIG_BB_LIFECYCLE_REASON_MAX 24
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct {
    char name[CONFIG_BB_LIFECYCLE_NAME_MAX];
    bool started;
    uint32_t inhibits[BB_LIFECYCLE_INHIBIT_WORDS];
    _Atomic uint32_t version;
} bb_lifecycle_service_t;

typedef struct {
    char name[CONFIG_BB_LIFECYCLE_REASON_MAX];
} bb_lifecycle_reason_slot_t;

typedef struct {
    bb_lifecycle_observer_fn cb;
    void *user;
    bool  async;  // false: bb_lifecycle_observe() -- fires inline on notify_all().
                  // true: bb_lifecycle_observe_async() (B1-1034) -- fires on the
                  // async drain task instead; see bb_lifecycle_async.c.
} bb_lifecycle_observer_slot_t;

static bb_lifecycle_service_t        s_services[CONFIG_BB_LIFECYCLE_MAX_SERVICES];
static _Atomic size_t                s_service_count;

static bb_lifecycle_reason_slot_t    s_reasons[CONFIG_BB_LIFECYCLE_MAX_REASONS];
static size_t                        s_reason_count; // only ever touched under s_lock

static bb_lifecycle_observer_slot_t  s_observers[CONFIG_BB_LIFECYCLE_MAX_OBSERVERS];
static _Atomic size_t                s_observer_count;

static bb_lock_t  s_lock;
static bb_once_t  s_lock_once = BB_ONCE_INIT;

static void init_lock(void *ctx)
{
    (void)ctx;
    bb_lock_config_t cfg = { .name = "bb_lifecycle", .category = "service" };
    bb_lock_init(&cfg, &s_lock);
}

static void ensure_lock(void)
{
    bb_once_run(&s_lock_once, init_lock, NULL);
}

// ---------------------------------------------------------------------------
// Pure helpers (no lock, no platform call -- host + device testable boundary)
// ---------------------------------------------------------------------------

// words/nwords are always a valid (non-NULL) caller-owned array in every
// production call site (bb_lifecycle_service_t.inhibits, sized
// BB_LIFECYCLE_INHIBIT_WORDS) -- only the out-of-range bit/nwords guard is
// a real, reachable safety check.
static void word_set(uint32_t *words, size_t nwords, uint8_t bit)
{
    size_t idx = (size_t)bit / 32;
    if (idx >= nwords) return;
    words[idx] |= (1u << (bit % 32));
}

static void word_clear(uint32_t *words, size_t nwords, uint8_t bit)
{
    size_t idx = (size_t)bit / 32;
    if (idx >= nwords) return;
    words[idx] &= ~(1u << (bit % 32));
}

static bool word_test(const uint32_t *words, size_t nwords, uint8_t bit)
{
    size_t idx = (size_t)bit / 32;
    if (idx >= nwords) return false;
    return (words[idx] & (1u << (bit % 32))) != 0;
}

static bool words_any_set(const uint32_t *words, size_t nwords)
{
    for (size_t i = 0; i < nwords; i++) {
        if (words[i]) return true;
    }
    return false;
}

// Effective state: STOPPED (not started) dominates PAUSED (any inhibit bit
// set) dominates RUNNING. Never stored -- always derived from (started,
// inhibits) so a stale cached state can never drift from reality.
static bb_lifecycle_state_t compute_state(bool started, const uint32_t *words, size_t nwords)
{
    if (!started) return BB_LIFECYCLE_STOPPED;
    if (words_any_set(words, nwords)) return BB_LIFECYCLE_PAUSED;
    return BB_LIFECYCLE_RUNNING;
}

#ifdef BB_LIFECYCLE_TESTING
void bb_lifecycle_word_set_for_test(uint32_t *words, size_t nwords, uint8_t bit)
{
    word_set(words, nwords, bit);
}

void bb_lifecycle_word_clear_for_test(uint32_t *words, size_t nwords, uint8_t bit)
{
    word_clear(words, nwords, bit);
}

bool bb_lifecycle_word_test_for_test(const uint32_t *words, size_t nwords, uint8_t bit)
{
    return word_test(words, nwords, bit);
}

bb_lifecycle_state_t bb_lifecycle_compute_state_for_test(bool started, const uint32_t *words, size_t nwords)
{
    return compute_state(started, words, nwords);
}
#endif

// strncpy + explicit NUL-termination (rules/embedded.md) -- this component's
// CMakeLists intentionally REQUIRES only bb_core (PRIV_REQUIRES bb_log), so
// it does not pull in bb_str for bb_strlcpy. Every call site passes a
// non-NULL fixed-size stack `dst` (dstsize == sizeof that array, always
// > 0) and a `src` already NULL-checked (-> BB_ERR_INVALID_ARG) by the
// public entry point before reaching here -- no defensive NULL/zero-size
// guard needed (and none would be reachable to test).
static void copy_truncated(char *dst, size_t dstsize, const char *src)
{
    strncpy(dst, src, dstsize - 1);
    dst[dstsize - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Reason intern table (global -- a string maps to the same bit across every
// service). Always called with s_lock held.
// ---------------------------------------------------------------------------

static bb_err_t reason_lookup_locked(const char *truncated, uint8_t *out_bit)
{
    for (size_t i = 0; i < s_reason_count; i++) {
        if (strcmp(s_reasons[i].name, truncated) == 0) {
            *out_bit = (uint8_t)i;
            return BB_OK;
        }
    }
    return BB_ERR_NOT_FOUND;
}

static bb_err_t reason_intern_locked(const char *reason, uint8_t *out_bit)
{
    char truncated[CONFIG_BB_LIFECYCLE_REASON_MAX];
    copy_truncated(truncated, sizeof(truncated), reason);

    if (reason_lookup_locked(truncated, out_bit) == BB_OK) {
        return BB_OK;
    }
    if (s_reason_count >= CONFIG_BB_LIFECYCLE_MAX_REASONS) {
        bb_log_w(TAG, "reason intern table full (max=%u), dropping '%s'",
                 (unsigned)CONFIG_BB_LIFECYCLE_MAX_REASONS, truncated);
        return BB_ERR_NO_SPACE;
    }
    size_t idx = s_reason_count++;
    copy_truncated(s_reasons[idx].name, sizeof(s_reasons[idx].name), truncated);
    *out_bit = (uint8_t)idx;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Generic emit sink (bb_lifecycle.h bb_lifecycle_set_emit). Bus-shaped
// (bb_emit_fn, bb_core/bb_emit.h). The generated invoke is forward-declared
// here (external linkage, matching what BB_CALLBACK_SLOT_VOID defines below)
// so it has an explicit prototype at every use site -- keeps this file clean
// under -Wmissing-prototypes.
// ---------------------------------------------------------------------------
void bb_lifecycle_emit_invoke(const char *topic, int32_t id, const void *payload, size_t size);

BB_CALLBACK_SLOT_VOID(emit, bb_emit_fn, bb_lifecycle_set_emit, bb_lifecycle_emit_invoke,
                      (const char *topic, int32_t id, const void *payload, size_t size),
                      (topic, id, payload, size))

// ---------------------------------------------------------------------------
// Handle validation + PUSH/PULL delivery
// ---------------------------------------------------------------------------

static bool svc_valid(bb_lifecycle_svc_t svc)
{
    if (svc < 0) return false;
    return (size_t)svc < atomic_load(&s_service_count);
}

static void notify_all(const bb_lifecycle_event_t *evt)
{
    // bb_lifecycle_observe()/bb_lifecycle_observe_async() both reject a NULL
    // cb (-> BB_ERR_INVALID_ARG) before ever appending a slot, so every slot
    // < s_observer_count carries a real callback -- no defensive NULL check
    // needed here. Async slots are skipped inline and handed to
    // bb_lifecycle_priv_async_notify() ONCE per transition (never
    // per-observer, never blocking) -- see bb_lifecycle_async.c.
    size_t n = atomic_load(&s_observer_count);
    bool any_async = false;
    for (size_t i = 0; i < n; i++) {
        if (s_observers[i].async) {
            any_async = true;
            continue;
        }
        s_observers[i].cb(evt, s_observers[i].user);
    }
    if (any_async) {
        bb_lifecycle_priv_async_notify(evt);
    }
    bb_lifecycle_emit_invoke(BB_LIFECYCLE_EVENT_TOPIC, evt->svc, evt, sizeof(*evt));
}

// Registry read only (no lock, no mutator) -- iterates the same slot table
// notify_all() reads, invoking only async-flagged slots. Called by the
// async drain task (bb_lifecycle_async.c) after bb_bqueue_receive(), and by
// bb_lifecycle_async_drain_dispatch_for_test() -- one code path, no mirror.
void bb_lifecycle_priv_invoke_async_slots(const bb_lifecycle_event_t *evt)
{
    size_t n = atomic_load(&s_observer_count);
    for (size_t i = 0; i < n; i++) {
        if (s_observers[i].async) {
            s_observers[i].cb(evt, s_observers[i].user);
        }
    }
}

static uint32_t commit_and_snapshot(bb_lifecycle_svc_t svc, bb_lifecycle_service_t *s,
                                    bb_lifecycle_state_t old_eff, uint8_t reason,
                                    bb_lifecycle_event_t *out_evt)
{
    bb_lifecycle_state_t new_eff = compute_state(s->started, s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS);
    uint32_t new_version = atomic_fetch_add(&s->version, 1) + 1;
    out_evt->svc       = (int32_t)svc;
    out_evt->inhibits  = s->inhibits[0];
    out_evt->version   = new_version;
    out_evt->old_state = (uint8_t)old_eff;
    out_evt->new_state = (uint8_t)new_eff;
    out_evt->reason    = reason;
    out_evt->_pad      = 0;
    return new_version;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_lifecycle_autoinit(void)
{
    ensure_lock();
    return BB_OK;
}

bb_err_t bb_lifecycle_register(const bb_lifecycle_config_t *cfg, bb_lifecycle_svc_t *out)
{
    if (!cfg || !cfg->name || !out) return BB_ERR_INVALID_ARG;

    char name[CONFIG_BB_LIFECYCLE_NAME_MAX];
    copy_truncated(name, sizeof(name), cfg->name);

    ensure_lock();
    bb_lock_lock(&s_lock);

    size_t count = atomic_load(&s_service_count);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(s_services[i].name, name) == 0) {
            bb_lock_unlock(&s_lock);
            return BB_ERR_CONFLICT;
        }
    }
    if (count >= CONFIG_BB_LIFECYCLE_MAX_SERVICES) {
        bb_lock_unlock(&s_lock);
        bb_log_w(TAG, "service table full (max=%u), dropping '%s'",
                 (unsigned)CONFIG_BB_LIFECYCLE_MAX_SERVICES, name);
        return BB_ERR_NO_SPACE;
    }

    bb_lifecycle_service_t *s = &s_services[count];
    memset(s, 0, sizeof(*s));
    copy_truncated(s->name, sizeof(s->name), name);
    s->started = false;
    atomic_store(&s->version, 0);

    atomic_store(&s_service_count, count + 1);
    bb_lock_unlock(&s_lock);

    *out = (bb_lifecycle_svc_t)count;
    return BB_OK;
}

bb_err_t bb_lifecycle_find(const char *name, bb_lifecycle_svc_t *out)
{
    if (!name || !out) return BB_ERR_INVALID_ARG;

    char truncated[CONFIG_BB_LIFECYCLE_NAME_MAX];
    copy_truncated(truncated, sizeof(truncated), name);

    ensure_lock();
    bb_lock_lock(&s_lock);
    size_t count = atomic_load(&s_service_count);
    bb_err_t err = BB_ERR_NOT_FOUND;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(s_services[i].name, truncated) == 0) {
            *out = (bb_lifecycle_svc_t)i;
            err = BB_OK;
            break;
        }
    }
    bb_lock_unlock(&s_lock);
    return err;
}

const char *bb_lifecycle_name(bb_lifecycle_svc_t svc)
{
    ensure_lock();
    bb_lock_lock(&s_lock);
    const char *name = svc_valid(svc) ? s_services[svc].name : "";
    bb_lock_unlock(&s_lock);
    return name;
}

bb_err_t bb_lifecycle_start(bb_lifecycle_svc_t svc)
{
    ensure_lock();
    bb_lock_lock(&s_lock);
    if (!svc_valid(svc)) {
        bb_lock_unlock(&s_lock);
        return BB_ERR_NOT_FOUND;
    }
    bb_lifecycle_service_t *s = &s_services[svc];
    bb_lifecycle_state_t old_eff = compute_state(s->started, s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS);
    if (s->started) {
        bb_lock_unlock(&s_lock); // idempotent success -- no notify, no version bump
        return BB_OK;
    }
    s->started = true;

    bb_lifecycle_event_t evt;
    commit_and_snapshot(svc, s, old_eff, BB_LIFECYCLE_REASON_NONE, &evt);
    bb_lock_unlock(&s_lock);

    notify_all(&evt);
    return BB_OK;
}

bb_err_t bb_lifecycle_stop(bb_lifecycle_svc_t svc)
{
    ensure_lock();
    bb_lock_lock(&s_lock);
    if (!svc_valid(svc)) {
        bb_lock_unlock(&s_lock);
        return BB_ERR_NOT_FOUND;
    }
    bb_lifecycle_service_t *s = &s_services[svc];
    bb_lifecycle_state_t old_eff = compute_state(s->started, s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS);
    if (!s->started) {
        bb_lock_unlock(&s_lock); // idempotent success -- no notify, no version bump
        return BB_OK;
    }
    s->started = false;
    memset(s->inhibits, 0, sizeof(s->inhibits)); // stop clears all inhibits

    bb_lifecycle_event_t evt;
    commit_and_snapshot(svc, s, old_eff, BB_LIFECYCLE_REASON_NONE, &evt);
    bb_lock_unlock(&s_lock);

    notify_all(&evt);
    return BB_OK;
}

bb_err_t bb_lifecycle_pause_assert(bb_lifecycle_svc_t svc, const char *reason)
{
    if (!reason) return BB_ERR_INVALID_ARG;

    ensure_lock();
    bb_lock_lock(&s_lock);
    if (!svc_valid(svc)) {
        bb_lock_unlock(&s_lock);
        return BB_ERR_NOT_FOUND;
    }
    bb_lifecycle_service_t *s = &s_services[svc];
    bb_lifecycle_state_t old_eff = compute_state(s->started, s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS);

    uint8_t bit;
    bb_err_t err = reason_intern_locked(reason, &bit);
    if (err != BB_OK) {
        bb_lock_unlock(&s_lock);
        return err;
    }
    if (word_test(s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS, bit)) {
        bb_lock_unlock(&s_lock); // idempotent success -- already asserted
        return BB_OK;
    }
    word_set(s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS, bit);

    bb_lifecycle_event_t evt;
    commit_and_snapshot(svc, s, old_eff, bit, &evt);
    bb_lock_unlock(&s_lock);

    notify_all(&evt);
    return BB_OK;
}

bb_err_t bb_lifecycle_pause_clear(bb_lifecycle_svc_t svc, const char *reason)
{
    if (!reason) return BB_ERR_INVALID_ARG;

    ensure_lock();
    bb_lock_lock(&s_lock);
    if (!svc_valid(svc)) {
        bb_lock_unlock(&s_lock);
        return BB_ERR_NOT_FOUND;
    }
    bb_lifecycle_service_t *s = &s_services[svc];
    bb_lifecycle_state_t old_eff = compute_state(s->started, s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS);

    char truncated[CONFIG_BB_LIFECYCLE_REASON_MAX];
    copy_truncated(truncated, sizeof(truncated), reason);

    uint8_t bit;
    if (reason_lookup_locked(truncated, &bit) != BB_OK) {
        bb_lock_unlock(&s_lock); // never interned -- no-op success
        return BB_OK;
    }
    if (!word_test(s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS, bit)) {
        bb_lock_unlock(&s_lock); // already clear -- idempotent success
        return BB_OK;
    }
    word_clear(s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS, bit);

    bb_lifecycle_event_t evt;
    commit_and_snapshot(svc, s, old_eff, bit, &evt);
    bb_lock_unlock(&s_lock);

    notify_all(&evt);
    return BB_OK;
}

bool bb_lifecycle_is_paused(bb_lifecycle_svc_t svc)
{
    ensure_lock();
    bb_lock_lock(&s_lock);
    bool paused = false;
    if (svc_valid(svc)) {
        bb_lifecycle_service_t *s = &s_services[svc];
        paused = compute_state(s->started, s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS) == BB_LIFECYCLE_PAUSED;
    }
    bb_lock_unlock(&s_lock);
    return paused;
}

bb_lifecycle_state_t bb_lifecycle_state(bb_lifecycle_svc_t svc)
{
    ensure_lock();
    bb_lock_lock(&s_lock);
    bb_lifecycle_state_t st = BB_LIFECYCLE_STOPPED;
    if (svc_valid(svc)) {
        bb_lifecycle_service_t *s = &s_services[svc];
        st = compute_state(s->started, s->inhibits, BB_LIFECYCLE_INHIBIT_WORDS);
    }
    bb_lock_unlock(&s_lock);
    return st;
}

uint32_t bb_lifecycle_version(bb_lifecycle_svc_t svc)
{
    if (!svc_valid(svc)) return 0; // lock-free -- s_service_count is atomic
    return atomic_load(&s_services[svc].version);
}

size_t bb_lifecycle_count(void)
{
    return atomic_load(&s_service_count);
}

size_t bb_lifecycle_inhibit_words(bb_lifecycle_svc_t svc, uint32_t *out, size_t max_words)
{
    if (!out || max_words == 0 || !svc_valid(svc)) return 0;

    ensure_lock();
    bb_lock_lock(&s_lock);
    // At the default MAX_REASONS<=32, BB_LIFECYCLE_INHIBIT_WORDS==1 and
    // max_words==0 is already rejected above, so the max_words<WORDS
    // true-branch is unreachable here -- expected missing branch coverage.
    size_t n = max_words < BB_LIFECYCLE_INHIBIT_WORDS ? max_words : BB_LIFECYCLE_INHIBIT_WORDS;
    memcpy(out, s_services[svc].inhibits, n * sizeof(uint32_t));
    bb_lock_unlock(&s_lock);
    return n;
}

const char *bb_lifecycle_reason_name(uint8_t bit)
{
    if (bit >= CONFIG_BB_LIFECYCLE_MAX_REASONS) return "";

    ensure_lock();
    bb_lock_lock(&s_lock);
    const char *name = (bit < s_reason_count) ? s_reasons[bit].name : "";
    bb_lock_unlock(&s_lock);
    return name;
}

bb_err_t bb_lifecycle_priv_observe_slot(bb_lifecycle_observer_fn cb, void *user, bool async)
{
    if (!cb) return BB_ERR_INVALID_ARG;

    ensure_lock();
    bb_lock_lock(&s_lock);
    size_t idx = atomic_load(&s_observer_count);
    if (idx >= CONFIG_BB_LIFECYCLE_MAX_OBSERVERS) {
        bb_lock_unlock(&s_lock);
        bb_log_w(TAG, "observer table full (max=%u), dropping registration",
                 (unsigned)CONFIG_BB_LIFECYCLE_MAX_OBSERVERS);
        return BB_ERR_NO_SPACE;
    }
    s_observers[idx].cb = cb;
    s_observers[idx].user = user;
    s_observers[idx].async = async;
    atomic_fetch_add(&s_observer_count, 1); // bump LAST -- publishes the slot
    bb_lock_unlock(&s_lock);
    return BB_OK;
}

bb_err_t bb_lifecycle_observe(bb_lifecycle_observer_fn cb, void *user)
{
    return bb_lifecycle_priv_observe_slot(cb, user, false);
}

#ifdef BB_LIFECYCLE_TESTING
void bb_lifecycle_reset_for_test(void)
{
    ensure_lock();
    bb_lock_lock(&s_lock);
    memset(s_services, 0, sizeof(s_services));
    atomic_store(&s_service_count, 0);
    memset(s_reasons, 0, sizeof(s_reasons));
    s_reason_count = 0;
    memset(s_observers, 0, sizeof(s_observers));
    atomic_store(&s_observer_count, 0);
    bb_lock_unlock(&s_lock);
    bb_lifecycle_set_emit(NULL);
}
#endif
