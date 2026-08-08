// bb_data_http pure core (B1-1033 PR-2, design KB 1443/1444). See
// bb_data_http.h for the full seam/invariant contract. This file has no
// FreeRTOS, httpd, or bb_data/bb_ws_server dependency -- everything it needs
// beyond bb_queue/bb_core/bb_registry/bb_str/bb_log is reached through the
// three injected seams (render/generation/send). ONE narrow exception
// (B1-1450): the shared EVENT ring's write-side lock below is a raw
// portMUX spinlock on ESP-IDF, self-contained behind its own #ifdef
// ESP_PLATFORM block -- see that block's own doc for why a raw spinlock
// rather than bb_lock_t.
#include "bb_data_http.h"
#include "bb_data_http_internal.h"

#include "bb_log.h"
#include "bb_registry.h"
#include "bb_str.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "bb_data_http";

// ---------------------------------------------------------------------------
// Kconfig -> C-default bridges. Every symbol here is a plain int Kconfig
// value (never undefined on ESP-IDF -- an int config always generates a
// concrete CONFIG_X), so the bare #ifndef fallback is safe (mirrors
// bb_event_routes_common.c's identical rationale) -- this is NOT the
// boolean-feature-flag shadowing trap the Kconfig-bridge convention warns
// against (see bb_lifecycle_async.c for that pattern).
// ---------------------------------------------------------------------------
#ifndef CONFIG_BB_DATA_HTTP_MAX_CLIENTS
#define CONFIG_BB_DATA_HTTP_MAX_CLIENTS 2
#endif
#ifndef CONFIG_BB_DATA_HTTP_OUTBOUND_CAPACITY
#define CONFIG_BB_DATA_HTTP_OUTBOUND_CAPACITY 8
#endif
#ifndef CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX
#define CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX 512
#endif
#ifndef CONFIG_BB_DATA_HTTP_OUTBOUND_MAX_BYTES
#define CONFIG_BB_DATA_HTTP_OUTBOUND_MAX_BYTES 4096
#endif
#ifndef CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY
#define CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY 16
#endif
#ifndef CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES
#define CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES 256
#endif
#ifndef CONFIG_BB_DATA_HTTP_SEND_FAIL_MAX
#define CONFIG_BB_DATA_HTTP_SEND_FAIL_MAX 3
#endif
// B1-1447: both default to CONFIG_BB_DATA_HTTP_MAX_CLIENTS (never a bare
// literal) so worst-case memory stays byte-identical to before this pool
// split -- see the Kconfig help text's identical rationale.
#ifndef CONFIG_BB_DATA_HTTP_MAX_POLL_CLIENTS
#define CONFIG_BB_DATA_HTTP_MAX_POLL_CLIENTS CONFIG_BB_DATA_HTTP_MAX_CLIENTS
#endif
#ifndef CONFIG_BB_DATA_HTTP_MAX_PUSH_CLIENTS
#define CONFIG_BB_DATA_HTTP_MAX_PUSH_CLIENTS CONFIG_BB_DATA_HTTP_MAX_CLIENTS
#endif
// A Kconfig int's `range 0 16` permits 0 (see BB_DATA_HTTP_MAX_POLL_CLIENTS/
// BB_DATA_HTTP_MAX_PUSH_CLIENTS's own doc: 0 proves that kind is genuinely
// optional at compile time), but a zero-length static array is not valid
// ISO C -- these back the pool arrays' actual declared size, while every
// loop bound below still uses the CONFIG_ value (or s_cfg's runtime-shrunk
// copy of it) directly, so a 0 cap still means "this pool never hands out a
// slot", never "index into 1 unadvertised slot".
#define BB_DATA_HTTP_POLL_POOL_BACKING (CONFIG_BB_DATA_HTTP_MAX_POLL_CLIENTS > 0 ? CONFIG_BB_DATA_HTTP_MAX_POLL_CLIENTS : 1)
#define BB_DATA_HTTP_PUSH_POOL_BACKING (CONFIG_BB_DATA_HTTP_MAX_PUSH_CLIENTS > 0 ? CONFIG_BB_DATA_HTTP_MAX_PUSH_CLIENTS : 1)

// ---------------------------------------------------------------------------
// Attach table (composition-root owned). Name-keyed small lookup table --
// reuses bb_registry rather than hand-rolling a second linear-scan dedupe
// (mirrors bb_data.c's own binding-table shape at the same ~8-entry scale).
// ---------------------------------------------------------------------------

typedef struct {
    bool                        in_use;
    char                        key[BB_DATA_HTTP_KEY_MAX];
    char                        topic[BB_DATA_HTTP_TOPIC_MAX];
    bb_data_http_replay_kind_t  kind;
} attach_slot_t;

static attach_slot_t s_attach[BB_DATA_HTTP_MAX_ATTACH];

BB_REGISTRY_DEFINE_TAGGED(s_attach_registry, BB_DATA_HTTP_MAX_ATTACH, "bb_data_http");

// First slot with in_use == false. Guaranteed to find one whenever
// bb_registry_count(&s_attach_registry) < BB_DATA_HTTP_MAX_ATTACH -- mirrors
// bb_data.c's find_free_slot() exactly (every successful register() here
// claims exactly one slot, neither table supports removal).
static attach_slot_t *find_free_attach_slot(void)
{
    for (size_t i = 0; i < BB_DATA_HTTP_MAX_ATTACH; i++) {
        if (!s_attach[i].in_use) return &s_attach[i];
    }
    return NULL;
}

// The attach-table index used for a key's dirty-mask bit / poll->seen_gen
// slot is always the registry's own bb_registry_get_by_index() position
// (register-only, no deregister -- so registration order IS a stable,
// permanent index), never the BSS array position in s_attach[] (which can
// differ once find_free_attach_slot() reuses a hole -- it never does today,
// but nothing below relies on the two coinciding).
// Loud guard (B1-1045 PR-4 fix): a binding whose desc->snap_size exceeds the
// shared render scratch (CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES) makes
// bb_data_render() return BB_ERR_NO_SPACE on EVERY sweep, forever -- with
// only a rate-limited WARN (bb_data_http_sweep_step()'s render-fail log).
// That silently starves the key's stream from first render onward (root
// cause of the "log" key never delivering a frame: 220B wire vs a 128B
// scratch). Checking at ATTACH time instead surfaces a misconfiguration at
// wire-up/boot as a loud, unmissable ERROR plus a rejected attach, rather
// than a permanently-failing stream nobody notices until they go looking.
// Runs BEFORE key/topic validation below (B1-1439 collapse preserves this
// ordering exactly -- see bb_data_http_attach_cfg_t's own snap_size doc).
bb_err_t bb_data_http_attach(const bb_data_http_attach_cfg_t *cfg)
{
    if (!cfg) return BB_ERR_INVALID_ARG;

    if (cfg->snap_size > (size_t)CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES) {
        bb_log_e(TAG, "attach('%s'): snap_size=%u exceeds render scratch=%u -- "
                "this key would render-fail (BB_ERR_NO_SPACE) every sweep, "
                "forever; not attaching -- bump "
                "CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES",
                cfg->key ? cfg->key : "(null)", (unsigned)cfg->snap_size,
                (unsigned)CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES);
        return BB_ERR_NO_SPACE;
    }

    const char *key = cfg->key;
    const char *topic = cfg->topic;
    if (!key || !key[0] || !topic || !topic[0]) return BB_ERR_INVALID_ARG;
    if (strlen(key) >= BB_DATA_HTTP_KEY_MAX) return BB_ERR_INVALID_ARG;
    if (strlen(topic) >= BB_DATA_HTTP_TOPIC_MAX) return BB_ERR_INVALID_ARG;

    attach_slot_t *slot = (attach_slot_t *)bb_registry_lookup(&s_attach_registry, key);
    if (!slot) {
        slot = find_free_attach_slot();
        if (!slot) return BB_ERR_NO_SPACE;

        bb_strlcpy(slot->key, key, sizeof(slot->key));

        bb_err_t rc = bb_registry_register(&s_attach_registry, slot->key, slot);
        if (rc != BB_OK) return rc;  // LCOV_EXCL_BR_LINE -- find_free_attach_slot()'s in_use scan and the registry's own capacity move in lockstep, ruling out NO_SPACE/duplicate-key; the lazy lock-init behind bb_registry_register() can still fail transiently (lock-unavailable), a real but not host-reproducible branch (see bb_registry.c) -- the checked return above is what propagates it.
        slot->in_use = true;
    }

    bb_strlcpy(slot->topic, topic, sizeof(slot->topic));
    slot->kind = cfg->kind;
    return BB_OK;
}

size_t bb_data_http_attach_count(void)
{
    return bb_registry_count(&s_attach_registry);
}

// ---------------------------------------------------------------------------
// Describe table (B1-1220). Deliberately independent of the attach table
// above -- see bb_data_http.h's doc comment for the rationale. A plain
// linear array + dedup scan (mirrors bb_openapi's own
// s_schema_registry/bb_openapi_register_schema() shape) rather than
// bb_registry: this table is never looked up by key at runtime (only walked
// in full via foreach), so a name-keyed lookup structure buys nothing here.
// ---------------------------------------------------------------------------

typedef struct {
    const char *key;
    const char *topic;
    const char *component_name;
    const char *schema_literal;
} describe_entry_t;

static describe_entry_t s_describe[BB_DATA_HTTP_MAX_DESCRIBE];
static size_t           s_describe_count;

bb_err_t bb_data_http_describe(const char *key, const char *topic,
                               const char *component_name,
                               const char *schema_literal)
{
    if (!key || !key[0] || !topic || !topic[0] ||
        !component_name || !component_name[0] ||
        !schema_literal || !schema_literal[0]) {
        return BB_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_describe_count; i++) {
        if (strcmp(s_describe[i].key, key) == 0) return BB_OK;  // dedup first-wins
    }

    if (s_describe_count >= BB_DATA_HTTP_MAX_DESCRIBE) {
        bb_log_e(TAG, "describe table full (cap=%d); dropping descriptor for key '%s'",
                 BB_DATA_HTTP_MAX_DESCRIBE, key);
        return BB_ERR_NO_SPACE;
    }

    s_describe[s_describe_count].key            = key;
    s_describe[s_describe_count].topic          = topic;
    s_describe[s_describe_count].component_name = component_name;
    s_describe[s_describe_count].schema_literal = schema_literal;
    s_describe_count++;
    return BB_OK;
}

void bb_data_http_describe_foreach(bb_data_http_describe_cb_t cb, void *ctx)
{
    if (!cb) return;
    for (size_t i = 0; i < s_describe_count; i++) {
        cb(s_describe[i].key, s_describe[i].topic,
           s_describe[i].component_name, s_describe[i].schema_literal, ctx);
    }
}

// True when client `c` should receive updates for a `kind`-kind key
// attached under `topic`: BOTH the existing topic-string rule (an empty
// topic_filter subscribes to everything; otherwise exact match only) AND
// `c`'s resolved subscribe_mask having `kind`'s bit set must hold (B1-1445)
// -- the two gates are ANDed, never substitutes for each other.
static bool client_subscribes(const bb_data_http_client_t *c, const char *topic,
                              bb_data_http_replay_kind_t kind)
{
    if ((c->subscribe_mask & (1u << kind)) == 0) return false;  // LCOV_EXCL_BR_LINE -- unreachable as false since B1-1446: every call site (the force-dirty walk, the STATE detect loop, and drain_client_events()) now pre-filters on this exact mask bit before ever reaching this call, so `kind`'s bit is always set here today. Kept as defense-in-depth (client_subscribes()'s own doc still promises the AND, independent of any one caller's pre-filter) against a future caller that doesn't pre-filter -- not dead code to delete.
    if (c->topic_filter[0] == '\0') return true;
    return strcmp(c->topic_filter, topic) == 0;
}

// ---------------------------------------------------------------------------
// Injected seams
// ---------------------------------------------------------------------------

static bb_data_http_render_fn     s_render_fn  = NULL;
static void                      *s_render_ctx = NULL;
static bb_data_http_generation_fn s_gen_fn     = NULL;
static void                      *s_gen_ctx    = NULL;
static bb_data_http_send_fn       s_send_fn    = NULL;
static void                      *s_send_ctx   = NULL;
static bb_data_http_abort_fn      s_abort_fn   = NULL;
static void                      *s_abort_ctx  = NULL;

// Cumulative render_fn failure count (bb_data_http_render_fail_count()) plus
// the cadence at which a failure is actually logged -- every failure still
// leaves the dirty bit set for retry (see bb_data_http_sweep_step()), but a
// persistently-failing render_fn must not spam the log on every sweep.
static uint32_t s_render_fail_count;
#define BB_DATA_HTTP_RENDER_FAIL_LOG_EVERY 32

// ---------------------------------------------------------------------------
// Shared EVENT ring (B1-1032, KB 1443/1444). A single bb_queue fed by every
// EVENT-kind attached key -- append-only, NOT coalesced (STATE's own
// dirty-mask coalescing does not apply here). Each push carries the
// originating attach-table index in the bb_queue entry's `id` field so a
// client's drain can resolve topic-filter membership.
//
// s_event_total_pushed is the running count of pushes since init/reset --
// NOT current ring occupancy (which shrinks on eviction). A ring entry's own
// global sequence number is derived positionally at drain time rather than
// stored (bb_queue's `id` slot only carries one value, already spent on the
// attach index): the oldest entry currently in the ring has global sequence
// (s_event_total_pushed - bb_queue_count(s_event_ring)).
//
// s_event_last_gen is the module-level (not per-client) last-seen generation
// per attach-table index, used to detect a new EVENT push is needed --
// mirrors STATE's poll->seen_gen role but scoped to the whole ring rather
// than any one client, since the ring itself is the single shared consumer
// of generation changes.
static bb_queue_t s_event_ring;
static uint32_t   s_event_total_pushed;
static uint32_t   s_event_last_gen[BB_DATA_HTTP_MAX_ATTACH];

// s_stale_discard_count (B1-1444): cumulative count of push_pump()'s COMMIT-
// step discards -- a successfully-rendered frame thrown away because a
// concurrent newer claim already superseded this generation (see
// push_pump()'s own step 4a doc). Guarded by the same s_event_ring_mux as
// s_event_total_pushed, incremented in the same locked compare-and-discard
// section rather than a second lock. Not a per-discard log: a persistently-
// racing producer can hit this path at high frequency, and logging every
// occurrence would be its own throughput problem -- the counter is the
// observability mechanism instead.
static uint32_t s_stale_discard_count;

// BB_DATA_HTTP_EVENT_ENTRY_MAX -- single source of truth for BOTH the event
// ring's max_entry_bytes (event_cfg, bb_data_http_init() below) AND
// bb_data_http_push_pump()'s ev_buf stack buffer, so the two structurally
// cannot drift apart independently (one macro, two use sites, rather than
// two copies of the same literal). This equality is safety-load-bearing,
// not just a size optimization -- see s_event_ring_mux's doc below for why.
#define BB_DATA_HTTP_EVENT_ENTRY_MAX CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX

// s_event_ring_mux (B1-1450, epic B1-1123): guards the CLAIM/PUSH protocol
// bb_data_http_push_pump() uses to make the shared EVENT ring safe for a
// second, concurrent caller -- see that function's own doc (bb_data_http.h)
// for the full claim/render/commit-or-revoke contract this lock backs.
// Same discipline as the espidf backend's own s_slots_mux/s_ws_slots_mux
// (bb_data_http_espidf.c): a portMUX spinlock, held only across bounded,
// non-blocking work -- a bb_queue_push, a plain integer read/write/
// compare -- NEVER across render_fn or any other call that can block.
// This is a raw portMUX rather than bb_lock_t deliberately: bb_lock_t is a
// blocking mutex (fine for a task that may sleep waiting for it), but this
// component's existing sibling locks in the same file family are already
// portMUX spinlocks for exactly this hold-time-bounded, no-blocking-inside
// pattern -- matching that established discipline here keeps one lock
// style per hazard class in this component rather than introducing a
// second, inconsistent one. Self-contained #ifdef ESP_PLATFORM block: this
// is the ONE place this otherwise-portable file reaches for a FreeRTOS
// header (see the file's own top-of-file doc), and it never leaks a
// platform type past this block -- BB_DATA_HTTP_EVENT_RING_LOCK/UNLOCK are
// plain macros, not exposed types. On host, the lock is a no-op: the host
// test harness is documented single-threaded (no pthreads driving
// concurrent callers), so there is nothing to guard against there, and a
// real host mutex would only cost cycles for no correctness benefit.
//
// HAZARD this section's held-work must never grow into: bb_queue_push()
// (platform/host/bb_queue/bb_queue.c) has its own bb_log_w/bb_log_d calls
// on two branches -- "len > max_entry" (rejected regardless of policy) and
// "ring full under BB_QUEUE_REJECT_NEW" -- and a log call is blocking
// FreeRTOS plumbing on ESP-IDF, which must never run inside a portMUX
// critical section. Neither branch can fire today ONLY because of two
// invariants held together here: (1) BB_DATA_HTTP_EVENT_ENTRY_MAX ties
// ev_len's upper bound (sizeof(ev_buf)) to event_cfg.max_entry_bytes, so
// "len > max_entry" is unreachable; (2) event_cfg.policy below is
// BB_QUEUE_EVICT_OLDEST with max_age == 0, so the ring never rejects a
// push for being full -- it evicts instead, silently, no log branch. A
// static_assert can't usefully pin either invariant (the entry-size one is
// already structural via the shared macro above, not a numeric coincidence
// to assert on; the policy one is a runtime enum value, not a compile-time
// constant this file can compare against a symbol tying it back to the
// lock). Re-verify this comment's claim by hand if event_cfg.policy above
// ever changes away from BB_QUEUE_EVICT_OLDEST, or if ev_buf's declaration
// in bb_data_http_push_pump() is ever changed to something other than
// BB_DATA_HTTP_EVENT_ENTRY_MAX.
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
static portMUX_TYPE s_event_ring_mux = portMUX_INITIALIZER_UNLOCKED;
#define BB_DATA_HTTP_EVENT_RING_LOCK()   portENTER_CRITICAL(&s_event_ring_mux)
#define BB_DATA_HTTP_EVENT_RING_UNLOCK() portEXIT_CRITICAL(&s_event_ring_mux)
#else
#define BB_DATA_HTTP_EVENT_RING_LOCK()   ((void)0)
#define BB_DATA_HTTP_EVENT_RING_UNLOCK() ((void)0)
#endif

void bb_data_http_set_render_fn(bb_data_http_render_fn fn, void *ctx)
{
    s_render_fn  = fn;
    s_render_ctx = ctx;
}

void bb_data_http_set_generation_fn(bb_data_http_generation_fn fn, void *ctx)
{
    s_gen_fn  = fn;
    s_gen_ctx = ctx;
}

void bb_data_http_set_send_fn(bb_data_http_send_fn fn, void *ctx)
{
    s_send_fn  = fn;
    s_send_ctx = ctx;
}

void bb_data_http_set_abort_fn(bb_data_http_abort_fn fn, void *ctx)
{
    s_abort_fn  = fn;
    s_abort_ctx = ctx;
}

// ---------------------------------------------------------------------------
// Config / lifecycle
// ---------------------------------------------------------------------------

static struct {
    size_t max_clients;
    size_t max_poll_clients;
    size_t max_push_clients;
    bool   initialized;
} s_cfg;

static bb_data_http_client_t s_clients[CONFIG_BB_DATA_HTTP_MAX_CLIENTS];

// ---------------------------------------------------------------------------
// STATE (poll) / EVENT (push) bookkeeping pools (B1-1447, epic B1-1123).
// Separately sized from s_clients above -- a client whose subscribe_mask
// excludes a kind never draws from that kind's pool at all (see
// bb_data_http_client_acquire() below), so the two pools can be tuned
// independently of the fd-table cap and of each other. Backing array size
// uses the *_BACKING macros (never 0 -- see their own doc) even when the
// Kconfig cap itself is 0; every loop bound below still uses the real
// (possibly runtime-shrunk via s_cfg) cap, so a 0-capacity pool still hands
// out zero slots.
// ---------------------------------------------------------------------------
static bb_data_http_poll_state_t s_poll_states[BB_DATA_HTTP_POLL_POOL_BACKING];
static bool                      s_poll_in_use[BB_DATA_HTTP_POLL_POOL_BACKING];
static bb_data_http_push_state_t s_push_states[BB_DATA_HTTP_PUSH_POOL_BACKING];
static bool                      s_push_in_use[BB_DATA_HTTP_PUSH_POOL_BACKING];

// First free poll-pool slot within the runtime-shrunk s_cfg.max_poll_clients
// bound (never the compile-time backing array size -- mirrors
// bb_data_http_client_acquire()'s own `s_cfg.max_clients`-bounded scan).
// NULL if the pool is exhausted (or its runtime cap is 0).
static bb_data_http_poll_state_t *poll_alloc(void)
{
    for (size_t i = 0; i < s_cfg.max_poll_clients; i++) {
        if (!s_poll_in_use[i]) {
            s_poll_in_use[i] = true;
            memset(&s_poll_states[i], 0, sizeof(s_poll_states[i]));
            return &s_poll_states[i];
        }
    }
    return NULL;
}

// No-op on NULL (mirrors bb_queue_destroy()'s own NULL-safe convention) --
// safe to call unconditionally on a client whose poll was never allocated.
static void poll_free(bb_data_http_poll_state_t *p)
{
    if (!p) return;
    s_poll_in_use[p - s_poll_states] = false;
}

// Mirror of poll_alloc() for the push pool.
static bb_data_http_push_state_t *push_alloc(void)
{
    for (size_t i = 0; i < s_cfg.max_push_clients; i++) {
        if (!s_push_in_use[i]) {
            s_push_in_use[i] = true;
            memset(&s_push_states[i], 0, sizeof(s_push_states[i]));
            return &s_push_states[i];
        }
    }
    return NULL;
}

// Mirror of poll_free() for the push pool.
static void push_free(bb_data_http_push_state_t *p)
{
    if (!p) return;
    s_push_in_use[p - s_push_states] = false;
}

bb_err_t bb_data_http_init(const bb_data_http_cfg_t *cfg)
{
    if (s_cfg.initialized) return BB_OK;

    size_t max_clients = (cfg && cfg->max_clients) ? cfg->max_clients : CONFIG_BB_DATA_HTTP_MAX_CLIENTS;
    if (max_clients > CONFIG_BB_DATA_HTTP_MAX_CLIENTS) return BB_ERR_INVALID_ARG;

    size_t ring_capacity = (cfg && cfg->event_ring_capacity) ? cfg->event_ring_capacity
                                                              : CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY;
    if (ring_capacity > CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY) return BB_ERR_INVALID_ARG;

    // B1-1447: same shrink-only-override contract as max_clients/
    // ring_capacity above -- see bb_data_http_cfg_t's doc (bb_data_http.h).
    size_t max_poll_clients = (cfg && cfg->max_poll_clients) ? cfg->max_poll_clients : CONFIG_BB_DATA_HTTP_MAX_POLL_CLIENTS;
    if (max_poll_clients > CONFIG_BB_DATA_HTTP_MAX_POLL_CLIENTS) return BB_ERR_INVALID_ARG;

    size_t max_push_clients = (cfg && cfg->max_push_clients) ? cfg->max_push_clients : CONFIG_BB_DATA_HTTP_MAX_PUSH_CLIENTS;
    if (max_push_clients > CONFIG_BB_DATA_HTTP_MAX_PUSH_CLIENTS) return BB_ERR_INVALID_ARG;

    bb_queue_cfg_t event_cfg = {
        .capacity_entries = ring_capacity,
        .max_entry_bytes  = BB_DATA_HTTP_EVENT_ENTRY_MAX,
        .policy           = BB_QUEUE_EVICT_OLDEST,
        .name             = "bbdhevt",
        .max_bytes        = 0,
        .max_age          = 0,
    };
    bb_queue_t event_ring;
    bb_err_t   event_err = bb_queue_create(&event_cfg, &event_ring);
    if (event_err != BB_OK) return event_err;

    // Every validation/allocation step above succeeded -- commit s_cfg (and
    // the rest of the module's live state) only now, never before, so a
    // failed init leaves s_cfg untouched (a caller can retry init() after
    // fixing the cause instead of observing a half-mutated config).
    s_cfg.max_clients      = max_clients;
    s_cfg.max_poll_clients = max_poll_clients;
    s_cfg.max_push_clients = max_push_clients;
    s_event_ring           = event_ring;
    s_event_total_pushed   = 0;
    s_stale_discard_count  = 0;
    memset(s_event_last_gen, 0, sizeof(s_event_last_gen));

    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        s_clients[i].in_use = false;
    }
    memset(s_poll_in_use, 0, sizeof(s_poll_in_use));
    memset(s_push_in_use, 0, sizeof(s_push_in_use));

    s_cfg.initialized = true;
    bb_log_i(TAG, "initialized: max_clients=%u event_ring_capacity=%u max_poll_clients=%u max_push_clients=%u",
             (unsigned)s_cfg.max_clients, (unsigned)ring_capacity,
             (unsigned)s_cfg.max_poll_clients, (unsigned)s_cfg.max_push_clients);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------

bb_err_t bb_data_http_client_acquire(const bb_data_http_client_cfg_t *cfg,
                                     bb_data_http_client_t **out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (!s_cfg.initialized) return BB_ERR_INVALID_STATE;
    // Mirrors bb_data_http_attach()'s own topic-length bound: an
    // over-length filter must be REJECTED, never silently truncated by the
    // bb_strlcpy below (a truncated filter can mis-subscribe a client to a
    // topic it never asked for).
    if (cfg->topic_filter && strlen(cfg->topic_filter) >= BB_DATA_HTTP_TOPIC_MAX) return BB_ERR_INVALID_ARG;

    for (size_t i = 0; i < s_cfg.max_clients; i++) {
        bb_data_http_client_t *c = &s_clients[i];
        if (c->in_use) continue;

        // Zero-default -> ALL (B1-1445): a cfg that never sets this
        // field resolves to the same "receive everything" behavior every
        // existing call site already had -- see bb_data_http_client_cfg_t's
        // subscribe_mask doc (bb_data_http.h) for why a bare 0 is
        // unambiguously safe here.
        uint32_t mask = cfg->subscribe_mask ? cfg->subscribe_mask : BB_DATA_HTTP_SUBSCRIBE_ALL;

        // Pool-first (B1-1447): resolve this client's STATE/EVENT
        // bookkeeping BEFORE touching `c` or creating its outbound queue --
        // a client with a kind bit set but no pool slot to back it must
        // never come into existence (see bb_data_http_client_acquire()'s
        // doc, bb_data_http.h: acquire fails BB_ERR_NO_SPACE cleanly rather
        // than returning a client with an unexpectedly-NULL poll/push). `c`
        // (the free fd-table slot found above) is left completely untouched
        // on either failure path below -- still available for a later
        // acquire attempt, no partial state to unwind.
        bb_data_http_poll_state_t *poll_state = NULL;
        if (mask & BB_DATA_HTTP_SUBSCRIBE_STATE) {
            poll_state = poll_alloc();
            if (!poll_state) return BB_ERR_NO_SPACE;
        }
        bb_data_http_push_state_t *push_state = NULL;
        if (mask & BB_DATA_HTTP_SUBSCRIBE_EVENT) {
            push_state = push_alloc();
            if (!push_state) {
                poll_free(poll_state);
                return BB_ERR_NO_SPACE;
            }
        }

        if (cfg->topic_filter) {
            bb_strlcpy(c->topic_filter, cfg->topic_filter, sizeof(c->topic_filter));
        } else {
            c->topic_filter[0] = '\0';
        }
        c->subscribe_mask = mask;
        c->poll           = poll_state;
        c->push           = push_state;
        if (c->push) {
            // Fresh EVENT clients start at the ring's current head -- they
            // receive only EVENTs pushed AFTER they connect, never ring
            // backlog (unlike STATE's fresh-render-on-connect below; see
            // bb_data_http_push_state_t's cursor doc). poll_alloc()/
            // push_alloc() already zero-initialize the block, so only the
            // one non-zero field needs setting here.
            c->push->cursor = s_event_total_pushed;
        }
        c->send_fail_count = 0;
        atomic_store(&c->pending_release, false);
        c->send_fn   = cfg->send_fn;
        c->send_ctx  = cfg->send_ctx;
        c->abort_fn  = cfg->abort_fn;
        c->abort_ctx = cfg->abort_ctx;

        c->outbound_max_bytes = CONFIG_BB_DATA_HTTP_OUTBOUND_MAX_BYTES;
        bb_queue_cfg_t qcfg = {
            .capacity_entries = CONFIG_BB_DATA_HTTP_OUTBOUND_CAPACITY,
            .max_entry_bytes  = CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX,
            .policy           = BB_QUEUE_EVICT_OLDEST,
            .name             = "bbdhttp",
            .max_bytes        = c->outbound_max_bytes,
            // No age budget: this pure core has no injected clock seam --
            // age-based eviction is left to a future PR if a real cadence
            // needs it. Byte budget alone still bounds worst-case memory.
            .max_age          = 0,
        };
        bb_err_t err = bb_queue_create(&qcfg, &c->outbound);
        if (err != BB_OK) {
            // `c` is still not `in_use` -- unwind the pool slots claimed
            // above before propagating the error, same "leave nothing
            // half-claimed" discipline as the pool-exhaustion returns above.
            poll_free(c->poll);
            push_free(c->push);
            c->poll = NULL;
            c->push = NULL;
            return err;
        }

        // Fresh-render-on-connect: force every subscribed STATE key dirty
        // now, independent of generation comparison. A key that has never
        // been touched (generation still at its initial value) would
        // otherwise never look "dirty" to the generation-diff detect logic
        // in bb_data_http_sweep_step() -- seen_gen defaults to 0 here, which
        // can legitimately equal an untouched key's real generation.
        //
        // Hoisted mask check (B1-1446, now a direct poll!=NULL check post-
        // B1-1447): a client with no poll block (mask excludes STATE, or
        // the poll pool was exhausted -- unreachable here, since exhaustion
        // already returned above) skips this walk entirely -- not just the
        // per-key client_subscribes() gate below the walk, but the walk
        // itself. Visiting it for a NULL-poll client would dereference
        // c->poll->dirty_mask.
        if (c->poll) {
            uint16_t count = bb_registry_count(&s_attach_registry);
            for (uint16_t k = 0; k < count; k++) {
                bb_registry_entry_t e;
                if (bb_registry_get_by_index(&s_attach_registry, k, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- k < count by construction
                attach_slot_t *slot = (attach_slot_t *)e.value;
                if (slot->kind != BB_DATA_HTTP_STATE) continue;
                if (!client_subscribes(c, slot->topic, slot->kind)) continue;
                c->poll->dirty_mask |= (1u << k);
            }
        }

        c->in_use = true;
        *out = c;
        return BB_OK;
    }
    return BB_ERR_NO_SPACE;
}

void bb_data_http_client_release(bb_data_http_client_t *c)
{
    if (!c) return;
    bb_queue_destroy(c->outbound);
    c->outbound = NULL;
    poll_free(c->poll);
    c->poll = NULL;
    push_free(c->push);
    c->push = NULL;
    c->in_use = false;
    atomic_store(&c->pending_release, false);
}

void bb_data_http_client_request_release(bb_data_http_client_t *c)
{
    if (!c) return;
    // The ONE field a foreign task may touch on a bb_data_http_client_t --
    // see its TASK OWNERSHIP doc (bb_data_http_internal.h) and this
    // function's own doc (bb_data_http.h). Every other field stays
    // exclusively owned by the task that calls bb_data_http_sweep_step();
    // this atomic store is what lets that stay true without a mutex.
    atomic_store(&c->pending_release, true);
}

size_t bb_data_http_active_client_count(void)
{
    size_t active = 0;
    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        if (s_clients[i].in_use) active++;
    }
    return active;
}

// True when client `c`'s own outbound queue has room for a `len`-byte push
// WITHOUT relying on outbound's own BB_QUEUE_EVICT_OLDEST auto-eviction. The
// EVENT drain path (unlike STATE's direct bb_queue_push() below) must never
// silently evict an already-queued frame to make room -- that would corrupt
// the append-only ordering guarantee without the client ever finding out
// (see bb_data_http_client_dropped_count()). bb_queue exposes no "would this
// push fit" query, so this checks entry-count AND byte-budget headroom using
// `c`'s own recorded outbound_max_bytes (see bb_data_http_client_t's doc).
static bool client_outbound_has_room(const bb_data_http_client_t *c, size_t len)
{
    if (bb_queue_count(c->outbound) >= bb_queue_capacity(c->outbound)) return false;
    return bb_queue_bytes_used(c->outbound) + len <= c->outbound_max_bytes;
}

// Sentinel id for the drop-marker's own bb_queue_push() below (B1-1123 PR-2).
// Legit ids pushed elsewhere in this file are attach-table indices
// 0..BB_DATA_HTTP_MAX_ATTACH-1 (bb_queue's id field is uint32_t throughout
// push/peek/peek_at -- no narrowing anywhere -- so this high sentinel is safe
// with zero type changes); the marker previously reused literal `0`, which
// collided with the real attach index 0.
#define BB_DATA_HTTP_DROP_MARKER_ID ((uint32_t)UINT32_MAX)

// Reserved key literal resolved for the drop-marker frame at the flush site
// (see resolve_outbound_key() below) instead of a real attach-table key.
// Never NULL: bb_data_http_send_fn's contract (bb_data_http.h) is that `key`
// is always a valid, NUL-terminated string -- NULL would be a per-consumer
// crash trap for every future send_fn (e.g. a naive MQTT topic lookup); this
// literal also doubles as a candidate SSE event-name for the marker in a
// later PR (out of scope here -- see bb_data_http.h's send_fn doc).
#define BB_DATA_HTTP_DROP_MARKER_KEY "dropped"

// Resolves the attach-table key for a queued outbound entry's `id` -- every
// real STATE/EVENT push's id is an attach-table index (see the STATE drain
// and EVENT ring-feed pushes below); the drop-marker sentinel
// (BB_DATA_HTTP_DROP_MARKER_ID) is checked FIRST, before the registry
// lookup, so the hot (real-frame) path pays only the one sentinel
// comparison -- never a range check folded into the registry lookup itself.
static const char *resolve_outbound_key(uint32_t id)
{
    if (id == BB_DATA_HTTP_DROP_MARKER_ID) return BB_DATA_HTTP_DROP_MARKER_KEY;

    bb_registry_entry_t e;
    if (bb_registry_get_by_index(&s_attach_registry, (uint16_t)id, &e) != BB_OK) {  // LCOV_EXCL_BR_LINE -- id always names a still-registered attach index (the attach table never shrinks, mirrors drain_client_events()'s identical rationale); unreachable in practice.
        // LCOV_EXCL_START
        return BB_DATA_HTTP_DROP_MARKER_KEY;
        // LCOV_EXCL_STOP
    }
    return ((attach_slot_t *)e.value)->key;
}

// Attempts to flush client `c`'s pending "{"dropped":N}" marker (queued as a
// JSON object, matching the shape of every other frame this queue carries --
// a client that JSON-parses every frame must never see a bare non-JSON
// token) into its outbound queue. A no-op when nothing is pending or when
// outbound has no room yet (the marker then stays pending for a later
// attempt). Called both unconditionally at the top of every
// drain_client_events() sweep -- so a marker set by a PRIOR sweep still
// reaches the wire even if the EVENT feed has since gone quiet and no new
// ring entry ever revisits this client -- and again mid-loop below, so a
// marker that becomes pending partway through THIS sweep still gets queued
// ahead of any surviving entries drained later in the same call.
static void try_flush_event_drop_marker(bb_data_http_client_t *c)
{
    if (!c->push->drop_marker_pending) return;

    char marker[24];
    int  n = snprintf(marker, sizeof(marker), "{\"dropped\":%" PRIu32 "}", c->push->dropped);
    if (n < 0 || (size_t)n >= sizeof(marker)) return;  // LCOV_EXCL_LINE -- uint32_t max always fits this buffer
    if (!client_outbound_has_room(c, (size_t)n)) return;

    bb_queue_push(c->outbound, marker, (size_t)n, 0, BB_DATA_HTTP_DROP_MARKER_ID);
    c->push->drop_marker_pending = false;
}

// Drains every shared-ring EVENT entry newer than `c->push->cursor` -- and
// matching `c`'s topic_filter -- into c's own outbound queue, advancing its
// cursor past each entry it examines (whether delivered, filtered out, or
// dropped for lack of outbound room). See bb_data_http_sweep_step()'s EVENT
// doc for the full contract; this never blocks or otherwise affects any
// other client -- one slow client only ever drops its OWN events.
// REQUIRES c->push != NULL (B1-1447) -- every call site gates on it first
// (see bb_data_http_sweep_step()'s drain phase); called otherwise, this
// dereferences a NULL pointer.
static void drain_client_events(bb_data_http_client_t *c)
{
    size_t   ring_count = bb_queue_count(s_event_ring);
    uint32_t oldest_seq = s_event_total_pushed - (uint32_t)ring_count;

    // Flush any marker a prior sweep left pending BEFORE anything else this
    // sweep does -- see try_flush_event_drop_marker()'s doc.
    try_flush_event_drop_marker(c);

    // Wraparound-safe: once s_event_total_pushed passes UINT32_MAX, a plain
    // `<` comparison on these uint32_t sequence numbers breaks. The signed-
    // difference idiom (int32_t)(a - b) stays correct across the wrap as
    // long as the true distance between a and b never exceeds INT32_MAX,
    // which holds here (ring/outbound capacities are tiny by comparison).
    if ((int32_t)(c->push->cursor - oldest_seq) < 0) {
        // The ring evicted entries this client never drained -- count the
        // whole gap as dropped and fast-forward the cursor: those sequence
        // numbers no longer exist in the ring to retry.
        c->push->dropped             += (oldest_seq - c->push->cursor);
        c->push->drop_marker_pending  = true;
        c->push->cursor               = oldest_seq;
    }

    for (size_t idx = 0; idx < ring_count; idx++) {
        uint32_t seq = oldest_seq + (uint32_t)idx;
        if ((int32_t)(seq - c->push->cursor) < 0) continue;  // already drained by an earlier sweep (wraparound-safe, see above)

        char     buf[CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX];
        size_t   len = 0;
        int64_t  ts  = 0;
        uint32_t id  = 0;  // attach-table index this entry was pushed under
        if (bb_queue_peek_at(s_event_ring, idx, buf, sizeof(buf), &len, &ts, &id) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- idx < ring_count by construction

        c->push->cursor = seq + 1;

        bb_registry_entry_t e;
        if (bb_registry_get_by_index(&s_attach_registry, (uint16_t)id, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- id always names a still-registered attach index (the attach table never shrinks); unreachable in practice.
        attach_slot_t *slot = (attach_slot_t *)e.value;

        if (!client_subscribes(c, slot->topic, slot->kind)) continue;

        try_flush_event_drop_marker(c);

        if (!client_outbound_has_room(c, len)) {
            c->push->dropped             += 1;
            c->push->drop_marker_pending  = true;
            continue;
        }

        bb_queue_push(c->outbound, buf, len, ts, id);
    }
}

// Ring-feed step (B1-1449, design KB 1443/1444, epic B1-1123): for every
// attached EVENT-kind key whose generation (via generation_fn) differs from
// the module's own last-seen generation for that key, calls render_fn and
// pushes the rendered bytes onto the single shared EVENT ring -- see this
// function's own doc (bb_data_http.h) for the full contract. Extracted out
// of bb_data_http_sweep_step()'s old single detect loop (B1-1449) so it can
// be called directly, decoupled from the 50ms broadcaster tick;
// sweep_step() still calls it in the same relative position (after STATE
// detect, before the per-client drain/flush loop), so the tick-driven path
// is unchanged.
//
// CLAIM/RENDER/COMMIT-OR-REVOKE protocol (B1-1450, epic B1-1123): this is
// what makes the function safe for a second, concurrent caller. Per key:
//   1. CLAIM (locked): compare gen to s_event_last_gen[k]. Equal means some
//      caller -- this one on a prior call, or a concurrent one just now --
//      already has this generation handled or in flight; skip. Otherwise
//      immediately store s_event_last_gen[k] = gen (remembering the prior
//      value) BEFORE render_fn runs, so a second concurrent caller that
//      reaches this same key's claim step next sees the NEW value and
//      skips -- only one caller ever wins the claim for a given
//      (key, generation) pair. This is the ONLY place s_event_last_gen[k]
//      is ever written for the no-render_fn degrade path too (folded into
//      the claim itself, see step 2) -- there is exactly one write site,
//      always under the lock, not a separate unguarded one.
//   2. No render_fn installed: the claim above already advanced past this
//      generation; nothing further to do (mirrors the old degrade-and-
//      advance behavior, now inherent in the claim rather than a second
//      write).
//   3. RENDER (unlocked): render_fn runs outside the lock -- it can block,
//      and per this component's contract may take arbitrarily long.
//   4a. COMMIT on render success (locked, compare-and-discard -- SYMMETRIC
//       with REVOKE below, not unconditional): re-check s_event_last_gen[k]
//       still equals the generation THIS caller claimed BEFORE pushing. If
//       it still matches, push the rendered frame and bump
//       s_event_total_pushed (s_event_last_gen[k] itself is NOT touched
//       here -- it already holds `gen`, set by the claim in step 1). If it
//       does NOT match, a second, concurrent caller has since claimed and
//       already committed a NEWER generation for this same key --
//       DISCARD this render (no push, no counter bump) instead of pushing
//       it. This check is required even though render_fn ran against a
//       generation THIS caller uniquely claimed: render_fn's own contract
//       is "render the key's CURRENT value", not a frozen per-generation
//       snapshot, so a slow render_fn that returns success late can easily
//       carry content at or past whatever a faster, later-claimed
//       concurrent render already pushed -- pushing it anyway would be a
//       second frame for one logical occurrence.
//   4b. REVOKE on render failure (locked, compare-and-restore): if
//       s_event_last_gen[k] still equals the generation THIS caller
//       claimed, restore it to the pre-claim value so a later call retries
//       this generation (same retry contract as before this protocol
//       existed). If it does NOT still equal that value, a second,
//       concurrent caller has since claimed a NEWER generation for this
//       same key -- leave it alone; blindly restoring would resurrect a
//       stale value and cause that other caller's already-in-flight (or
//       already-committed) work to be spuriously redone.
//
//       ABA note on this compare-and-restore: it is possible for a THIRD
//       caller's own earlier revoke to have coincidentally restored
//       s_event_last_gen[k] back to exactly the value THIS caller claimed,
//       making this compare pass when the "true" chain of custody would
//       say it shouldn't (e.g. A claims g1 and stalls; B claims g2, fails,
//       revokes to g1; A later fails too, sees g1 -- its own claimed value,
//       coincidentally restored by B -- and restores to g0). This is
//       provably self-healing, not a correctness gap: generations are
//       monotonic and never repeat, so any wrongly-reverted marker is
//       exceeded by the very next real generation change, which re-triggers
//       a fresh claim for that key. The only cost is a redundant re-render
//       of a generation that may already be reflected in a later push, not
//       a permanently lost event or a stuck marker -- deliberately not
//       worth an epoch/token scheme to close. PRECONDITION: this healing
//       depends on this function continuing to be invoked periodically by
//       every caller sharing this key (the broadcaster's existing
//       sweep-tick cadence, and whatever cadence a second caller uses) --
//       a caller wired to invoke this function only on-demand does not get
//       the self-healing guarantee for free; see this function's own doc
//       (bb_data_http.h) for the full precondition.
//
//       SUSTAINED-CHURN NOTE: under continuous generation churn for one
//       key faster than either caller's render_fn completes, COMMIT's
//       compare-and-discard can keep discarding every render for that key
//       -- throughput can drop to zero while churn continues. EXPECTED
//       under this component's coalesce-to-latest contract (render_fn
//       always renders CURRENT value, never a frozen snapshot), not a lock
//       defect -- see this function's own doc (bb_data_http.h) for the
//       full callout.
//
// Why this closes the double-delivery hazard a partial (push-only) lock
// left open: without locking the CLAIM (and, symmetrically, the COMMIT)
// step, two concurrent callers could both read the same stale
// s_event_last_gen[k], both decide to render, and both push -- two ring
// entries for one logical occurrence, with no downstream dedup (ring
// entries carry no generation, only the attach index). Locking only the
// final push prevented torn writes but not that double-decide/double-
// render/double-push outcome; locking the claim alone closes the
// same-(key,generation) case but NOT the cross-generation case (a slow
// claimant's stale-but-successful render landing after a faster,
// later-claimed concurrent render already pushed) -- COMMIT's own
// compare-and-discard closes that remaining gap by re-validating the claim
// immediately before the push, not just at claim time.
void bb_data_http_push_pump(void)
{
    if (!s_gen_fn) return;

    uint16_t attach_count = bb_registry_count(&s_attach_registry);
    for (uint16_t k = 0; k < attach_count; k++) {
        bb_registry_entry_t e;
        if (bb_registry_get_by_index(&s_attach_registry, k, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- k < attach_count by construction
        attach_slot_t *slot = (attach_slot_t *)e.value;
        if (slot->kind != BB_DATA_HTTP_EVENT) continue;  // STATE keys are handled by sweep_step()'s own detect phase

        uint32_t gen = 0;
        if (s_gen_fn(slot->key, &gen, s_gen_ctx) != BB_OK) continue;

        // Step 1: CLAIM. Locked check-and-set -- see this function's own
        // doc above for why this must be atomic with the generation
        // compare, not just the eventual push.
        BB_DATA_HTTP_EVENT_RING_LOCK();
        if (gen == s_event_last_gen[k]) {
            BB_DATA_HTTP_EVENT_RING_UNLOCK();
            continue;  // already handled: by us last call, or a concurrent caller just now
        }
        uint32_t prev_gen = s_event_last_gen[k];
        s_event_last_gen[k] = gen;  // claimed
        BB_DATA_HTTP_EVENT_RING_UNLOCK();

        if (!s_render_fn) {
            // Step 2: nothing to retry -- the claim above already advanced
            // past this generation (see this function's own doc, step 2).
            continue;
        }

        // Step 3: RENDER, unlocked -- render_fn can block; must never run
        // under s_event_ring_mux.
        char     ev_buf[BB_DATA_HTTP_EVENT_ENTRY_MAX];
        size_t   ev_len    = 0;
        bb_err_t render_rc = s_render_fn(slot->key, ev_buf, sizeof(ev_buf), &ev_len, s_render_ctx);
        if (render_rc != BB_OK) {
            // Step 4b: REVOKE (compare-and-restore) -- see this function's
            // own doc above for why the compare guards against clobbering
            // a newer concurrent claim.
            s_render_fail_count++;
            if (s_render_fail_count == 1 ||
                (s_render_fail_count % BB_DATA_HTTP_RENDER_FAIL_LOG_EVERY) == 0) {
                bb_log_w(TAG, "event render failed for key '%s': %d (total=%" PRIu32 ")",
                        slot->key, (int)render_rc, s_render_fail_count);
            }
            BB_DATA_HTTP_EVENT_RING_LOCK();
            if (s_event_last_gen[k] == gen) {
                s_event_last_gen[k] = prev_gen;
            }
            BB_DATA_HTTP_EVENT_RING_UNLOCK();
            continue;
        }

        // Step 4a: COMMIT -- symmetric with REVOKE (step 4b): re-check the
        // claim is still ours BEFORE pushing, same compare, same lock. A
        // SLOW render_fn can return success after a NEWER concurrent claim
        // has already committed fresher state for this key (render_fn's
        // contract is "render CURRENT value", not a per-generation
        // snapshot, so this stale render's payload plausibly reflects that
        // newer state too) -- pushing it anyway would be a second frame
        // for one logical occurrence, the exact double-delivery hazard
        // this protocol exists to close. If s_event_last_gen[k] still
        // equals `gen`, this claim is still current: push and bump the
        // counter as before. If it does not, DISCARD this render (no
        // push, no counter bump) -- a newer claim already reflects
        // fresher state.
        BB_DATA_HTTP_EVENT_RING_LOCK();
        if (s_event_last_gen[k] == gen) {
            bb_queue_push(s_event_ring, ev_buf, ev_len, 0, (uint32_t)k);
            s_event_total_pushed++;
        } else {
            s_stale_discard_count++;
        }
        BB_DATA_HTTP_EVENT_RING_UNLOCK();
    }
}

// ---------------------------------------------------------------------------
// Sweep -- see bb_data_http.h's bb_data_http_sweep_step() doc for the full
// two-phase detect/drain contract and the lost-wakeup ordering invariant.
// ---------------------------------------------------------------------------

void bb_data_http_sweep_step(void)
{
    uint16_t attach_count = bb_registry_count(&s_attach_registry);

    // ---- Detect phase: runs to completion for every key/client BEFORE any
    // STATE render call. A generation bump that happens later, during this
    // same pass's render calls (see the drain phase below), is therefore
    // invisible here and gets correctly picked up on the NEXT sweep_step()
    // call instead of being silently absorbed. EVENT-kind keys are skipped
    // here (see bb_data_http_push_pump(), called below) -- the shared ring
    // has no per-client dirty state to coalesce against, so there is
    // nothing to defer to a later drain phase for the ring-feed step itself
    // (per-client draining of the ring still happens below, in the main
    // per-client loop).
    if (s_gen_fn) {
        // Hoisted poll!=NULL check (B1-1446, now a direct pointer check post-
        // B1-1447): which in-use clients even want STATE delivery at all,
        // computed ONCE per client here rather than re-derived on every
        // (key, client) pair inside the STATE branch of the loop below -- a
        // push-only (EVENT-only, poll==NULL) client is then skipped from the
        // inner client loop entirely, for every STATE key, without ever
        // dereferencing its (absent) poll block. `poll != NULL` is exactly
        // equivalent to the mask check this replaces (poll is allocated iff
        // subscribe_mask includes BB_DATA_HTTP_SUBSCRIBE_STATE -- see
        // bb_data_http_client_acquire()) but is the direct, NULL-safety-
        // first form: reaching c->poll->seen_gen[k] for a NULL-poll client
        // here would otherwise be a NULL dereference. Skip-before-visit here
        // is what makes that split safe.
        bool state_eligible[CONFIG_BB_DATA_HTTP_MAX_CLIENTS];
        for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
            state_eligible[i] = s_clients[i].in_use && s_clients[i].poll != NULL;
        }

        for (uint16_t k = 0; k < attach_count; k++) {
            bb_registry_entry_t e;
            if (bb_registry_get_by_index(&s_attach_registry, k, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- k < attach_count by construction
            attach_slot_t *slot = (attach_slot_t *)e.value;
            if (slot->kind == BB_DATA_HTTP_EVENT) continue;  // handled by bb_data_http_push_pump() below

            uint32_t gen = 0;
            if (s_gen_fn(slot->key, &gen, s_gen_ctx) != BB_OK) continue;

            for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
                if (!state_eligible[i]) continue;
                bb_data_http_client_t *c = &s_clients[i];
                if (!client_subscribes(c, slot->topic, slot->kind)) continue;
                if (c->poll->seen_gen[k] == gen) continue;  // unchanged since last render

                c->poll->seen_gen[k] = gen;
                c->poll->dirty_mask |= (1u << k);
            }
        }
    }

    // ---- Ring feed: pushes every changed EVENT-kind key's rendered bytes
    // onto the shared ring -- see bb_data_http_push_pump()'s own doc above.
    // Called here, in the same relative position the old inline ring-feed
    // branch occupied (after STATE detect, before per-client drain/flush),
    // so this call is inert for the tick-driven path -- sweep_step()'s own
    // observable behavior is unchanged; push_pump() is additionally
    // independently callable, ahead of the 50ms broadcaster tick (B1-1449).
    bb_data_http_push_pump();

    // ---- Drain phase: per client, for each dirty STATE key, render THEN
    // clear the bit -- and only on render SUCCESS. A missing render_fn has
    // nothing to retry, so it clears unconditionally; a render_fn that fails
    // leaves the bit set so the key is retried next sweep_step() instead of
    // being silently dropped (see bb_data_http_sweep_step()'s doc comment
    // and the render-failure-retry host test). Every EVENT-participating
    // active client (c->push != NULL) -- dirty or not -- also has
    // drain_client_events() called on it (EVENT delivery is driven by the
    // shared ring's own state, not this client's own poll->dirty_mask).
    // Once both are resolved, the outbound queue is flushed via send_fn.
    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        bb_data_http_client_t *c = &s_clients[i];
        if (!c->in_use) continue;

        // Deferred-reap (B1-1424 HIGH fix): a foreign task (e.g. an async
        // transport's own disconnect callback) may have called
        // bb_data_http_client_request_release() on `c` since this task's
        // last sweep_step() call -- reap it FIRST, before touching any
        // other field, so nothing below this point ever operates on a
        // client mid-teardown. bb_data_http_client_release() is safe to
        // call here specifically because reaping only ever happens on the
        // single task that owns sweep_step() -- see
        // bb_data_http_client_t's TASK OWNERSHIP doc.
        if (atomic_load(&c->pending_release)) {
            bb_data_http_client_release(c);
            continue;
        }

        // c->poll == NULL for an EVENT-only client (B1-1447) -- the guard
        // below is the drain phase's own equivalent of state_eligible[]
        // above, gating on the pointer directly since there is no dirty
        // mask to read at all without it.
        if (c->poll && c->poll->dirty_mask != 0) {
            for (uint16_t k = 0; k < attach_count; k++) {
                uint32_t bit = (1u << k);
                if ((c->poll->dirty_mask & bit) == 0) continue;

                if (!s_render_fn) {
                    // No render seam installed -- nothing to retry, so
                    // degrade gracefully by clearing (see
                    // bb_data_http_set_render_fn()).
                    c->poll->dirty_mask &= ~bit;
                    continue;
                }

                bb_registry_entry_t e;
                if (bb_registry_get_by_index(&s_attach_registry, k, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- k < attach_count by construction
                attach_slot_t *slot = (attach_slot_t *)e.value;

                char     buf[CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX];
                size_t   out_len   = 0;
                bb_err_t render_rc = s_render_fn(slot->key, buf, sizeof(buf), &out_len, s_render_ctx);
                if (render_rc != BB_OK) {
                    // Leave the bit set: this key is retried on the next
                    // sweep_step() call rather than silently dropped. Rate-limit
                    // the log so a persistently-failing render_fn doesn't spam
                    // it every sweep, while still making the condition
                    // observable via bb_data_http_render_fail_count().
                    s_render_fail_count++;
                    if (s_render_fail_count == 1 ||
                        (s_render_fail_count % BB_DATA_HTTP_RENDER_FAIL_LOG_EVERY) == 0) {
                        bb_log_w(TAG, "render failed for key '%s': %d (total=%" PRIu32 ")",
                                slot->key, (int)render_rc, s_render_fail_count);
                    }
                    continue;
                }

                // Clear ONLY on render success -- see the doc comment above.
                c->poll->dirty_mask &= ~bit;

                // render_fn's own `cap` argument (sizeof(buf)) always equals the
                // outbound queue's configured max_entry_bytes, so out_len can
                // never exceed it -- push() can only fail here on a genuinely
                // oversized entry, which is unreachable given that invariant.
                bb_err_t push_err = bb_queue_push(c->outbound, buf, out_len, 0, (uint32_t)k);
                if (push_err != BB_OK) {  // LCOV_EXCL_BR_LINE -- unreachable, see comment above
                    // LCOV_EXCL_START
                    bb_log_w(TAG, "client outbound push failed for key '%s': %d", slot->key, (int)push_err);
                    // LCOV_EXCL_STOP
                }
            }
        }

        // Non-participation (B1-1446, now a direct push!=NULL check post-
        // B1-1447): a client with no push block (mask excludes EVENT)
        // never calls drain_client_events() at all -- that function
        // REQUIRES c->push != NULL (unconditionally advances its cursor and
        // drop-accounting off the shared ring's own state regardless of any
        // per-entry client_subscribes() filtering inside it), so a
        // STATE-only (push==NULL) client must not be visited here even
        // once.
        if (c->push) {
            drain_client_events(c);
        }

        // Per-client-then-global resolution (B1-1123 PR-1, see
        // bb_data_http.h's PER-CONSUMER SEAMS doc): a client acquired with
        // its own send_fn/abort_fn (bb_data_http_client_cfg_t) uses that
        // seam; otherwise it falls back to the module-wide default installed
        // via bb_data_http_set_send_fn()/bb_data_http_set_abort_fn() -- the
        // same fallback every existing HTTP client (which never sets the
        // per-client override) already relied on before this PR.
        bb_data_http_send_fn  send_fn  = c->send_fn  ? c->send_fn  : s_send_fn;
        void                 *send_ctx = c->send_fn  ? c->send_ctx : s_send_ctx;
        if (!send_fn) continue;

        // Flush phase (B1-1424/B1-1429): drains c->outbound oldest-first,
        // one send_fn call per queued frame, for as long as each call
        // succeeds -- see bb_data_http_sweep_step()'s flush-contract doc
        // (bb_data_http.h) for the full rationale. send_rc drives one of
        // three outcomes:
        //
        //   - BB_OK: pop and continue to the next queued frame (a client
        //     with several frames queued still catches up within one sweep
        //     as long as every send succeeds).
        //   - BB_ERR_TIMEOUT (retriable, nothing transmitted): the frame
        //     stays at the head of the queue for a retry next
        //     sweep_step() call, and this client's flush STOPS for the
        //     rest of THIS call (`break`, never `continue`) -- both to
        //     preserve delivery order (a later frame must never reach the
        //     wire ahead of one still queued in front of it) and to keep
        //     one stalled client's per-sweep cost bounded. Once
        //     CONFIG_BB_DATA_HTTP_SEND_FAIL_MAX consecutive retriable
        //     failures land on the SAME head-of-queue frame, that frame is
        //     dropped (popped without a further attempt) and the counter
        //     resets -- but the flush STILL stops for this sweep rather
        //     than immediately attempting the next queued frame: a
        //     bound-exceeded drop is exactly as terminal for this sweep as
        //     an under-bound failure is, it just also advances the queue.
        //   - Any other non-BB_OK code (fatal): the client is released
        //     immediately via the installed abort_fn (or
        //     bb_data_http_client_release() directly if none is installed)
        //     and the flush stops -- there is no client left to flush for.
        char    frame[CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX];
        size_t  frame_len = 0;
        int64_t ts        = 0;
        uint32_t id       = 0;
        while (bb_queue_peek_oldest(c->outbound, frame, sizeof(frame), &frame_len, &ts, &id) == BB_OK) {
            bb_err_t send_rc = send_fn(resolve_outbound_key(id), c, frame, frame_len, send_ctx);
            if (send_rc == BB_OK) {
                c->send_fail_count = 0;
                bb_queue_pop_oldest(c->outbound);
                continue;
            }

            if (send_rc == BB_ERR_TIMEOUT) {
                c->send_fail_count++;
                if (c->send_fail_count < CONFIG_BB_DATA_HTTP_SEND_FAIL_MAX) {
                    break;  // retry this same frame next sweep_step() -- never loop within this sweep
                }
                bb_log_w(TAG, "send failed %" PRIu32 " consecutive times for key '%s' (client=%p), dropping frame",
                        c->send_fail_count, resolve_outbound_key(id), (void *)c);
                bb_queue_pop_oldest(c->outbound);
                c->send_fail_count = 0;
                // Bound exceeded on this frame only -- but still stop for
                // THIS sweep (see the doc above) rather than immediately
                // attempting the next queued frame; it is picked up on the
                // next sweep_step() call instead.
                break;
            }

            // Fatal: send_fn cannot rule out that some bytes already
            // reached the peer with no way to resync -- see
            // bb_data_http_send_fn's return contract. Never retried; the
            // frame is left in the queue (irrelevant -- the client is
            // about to be released, which destroys the queue too).
            bb_log_w(TAG, "send failed fatally for key '%s' (client=%p): %d, aborting client",
                    resolve_outbound_key(id), (void *)c, (int)send_rc);
            bb_data_http_abort_fn abort_fn  = c->abort_fn  ? c->abort_fn  : s_abort_fn;
            void                 *abort_ctx = c->abort_fn  ? c->abort_ctx : s_abort_ctx;
            if (abort_fn) {
                abort_fn(c, abort_ctx);
            } else {
                bb_data_http_client_release(c);
            }
            break;
        }
    }
}

size_t bb_data_http_render_fail_count(void)
{
    return s_render_fail_count;
}

uint32_t bb_data_http_stale_discard_count(void)
{
    return s_stale_discard_count;
}

uint32_t bb_data_http_client_dropped_count(const bb_data_http_client_t *c)
{
    return (c && c->push) ? c->push->dropped : 0;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_DATA_HTTP_TESTING
void bb_data_http_reset_for_test(void)
{
    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        if (s_clients[i].in_use) bb_data_http_client_release(&s_clients[i]);
    }
    // Defensive belt-and-suspenders (B1-1447): every live client's release
    // above already frees its own poll/push pool slot, but clear both pools'
    // in_use bitmaps outright too, same full-reset discipline as the attach/
    // describe tables below -- a future acquire-path bug that leaks a pool
    // slot without a corresponding live client must not silently poison
    // every later test in the same process.
    memset(s_poll_in_use, 0, sizeof(s_poll_in_use));
    memset(s_push_in_use, 0, sizeof(s_push_in_use));
    memset(s_attach, 0, sizeof(s_attach));
    bb_registry_reset(&s_attach_registry);
    memset(s_describe, 0, sizeof(s_describe));
    s_describe_count = 0;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_render_fn = NULL; s_render_ctx = NULL;
    s_gen_fn    = NULL; s_gen_ctx    = NULL;
    s_send_fn   = NULL; s_send_ctx   = NULL;
    s_abort_fn  = NULL; s_abort_ctx  = NULL;
    s_render_fail_count = 0;

    bb_queue_destroy(s_event_ring);
    s_event_ring = NULL;
    s_event_total_pushed = 0;
    s_stale_discard_count = 0;
    memset(s_event_last_gen, 0, sizeof(s_event_last_gen));
}

uint32_t bb_data_http_client_dirty_mask_for_test(const bb_data_http_client_t *c)
{
    return (c && c->poll) ? c->poll->dirty_mask : 0;
}

uint32_t bb_data_http_client_seen_gen_for_test(const bb_data_http_client_t *c, size_t idx)
{
    if (!c || !c->poll || idx >= BB_DATA_HTTP_MAX_ATTACH) return 0;
    return c->poll->seen_gen[idx];
}

size_t bb_data_http_client_outbound_count_for_test(const bb_data_http_client_t *c)
{
    return c ? bb_queue_count(c->outbound) : 0;
}

uint32_t bb_data_http_client_send_fail_count_for_test(const bb_data_http_client_t *c)
{
    return c ? c->send_fail_count : 0;
}

uint32_t bb_data_http_client_event_cursor_for_test(const bb_data_http_client_t *c)
{
    return (c && c->push) ? c->push->cursor : 0;
}

// Current occupancy of the shared EVENT ring (B1-1449) -- bb_data_http_push_pump()'s
// direct-call test seam, so a test can assert the ring itself gained an
// entry without going through a client's own drained cursor.
size_t bb_data_http_event_ring_count_for_test(void)
{
    return s_event_ring ? bb_queue_count(s_event_ring) : 0;
}

// key -> attach-table registry index, test-only. Mirrors the same linear
// scan bb_data_http_push_pump() and friends already do internally (the
// attach-table index is registration order, not stored on the slot itself
// -- see bb_data_http_attach()'s own doc above). Returns true and
// writes *out_idx on a match, false (leaving *out_idx untouched) if key is
// not attached.
static bool find_attach_index_for_test(const char *key, uint16_t *out_idx)
{
    uint16_t count = bb_registry_count(&s_attach_registry);
    for (uint16_t k = 0; k < count; k++) {  // LCOV_EXCL_BR_LINE -- every test-only caller passes an already-attached key, so this loop always returns from inside before k reaches count
        bb_registry_entry_t e;
        if (bb_registry_get_by_index(&s_attach_registry, k, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- k < count by construction
        attach_slot_t *slot = (attach_slot_t *)e.value;
        if (strcmp(slot->key, key) == 0) {
            *out_idx = k;
            return true;
        }
    }
    return false;  // LCOV_EXCL_LINE -- every test-only caller passes an already-attached key
}

// B1-1450: current value of s_event_last_gen for `key`'s attach index.
// Returns 0 if key is not attached (mirrors every other for_test
// accessor's NULL-safe zero-return convention).
uint32_t bb_data_http_event_last_gen_for_test(const char *key)
{
    uint16_t idx = 0;
    if (!find_attach_index_for_test(key, &idx)) return 0;  // LCOV_EXCL_LINE -- every test-only caller passes an already-attached key
    uint32_t gen;
    BB_DATA_HTTP_EVENT_RING_LOCK();
    gen = s_event_last_gen[idx];
    BB_DATA_HTTP_EVENT_RING_UNLOCK();
    return gen;
}

// B1-1450: directly overwrite s_event_last_gen for `key`'s attach index,
// bypassing the claim/commit/revoke protocol entirely -- lets a host test
// simulate a second, concurrent bb_data_http_push_pump() caller claiming a
// NEWER generation for the SAME key while THIS caller's own render_fn call
// is still in flight (the compare-and-restore revoke's "leave it alone"
// branch, see that function's own doc), which the single-threaded host
// harness cannot otherwise reach. No-op if key is not attached.
void bb_data_http_event_last_gen_set_for_test(const char *key, uint32_t gen)
{
    uint16_t idx = 0;
    if (!find_attach_index_for_test(key, &idx)) return;  // LCOV_EXCL_LINE -- every test-only caller passes an already-attached key
    BB_DATA_HTTP_EVENT_RING_LOCK();
    s_event_last_gen[idx] = gen;
    BB_DATA_HTTP_EVENT_RING_UNLOCK();
}

bool bb_data_http_client_pending_release_for_test(const bb_data_http_client_t *c)
{
    return c ? atomic_load(&c->pending_release) : false;
}

bool bb_data_http_client_poll_is_null_for_test(const bb_data_http_client_t *c)
{
    return !c || c->poll == NULL;
}

bool bb_data_http_client_push_is_null_for_test(const bb_data_http_client_t *c)
{
    return !c || c->push == NULL;
}
#endif

// ---------------------------------------------------------------------------
// GET /api/events route descriptor (B1-1215) -- see bb_data_http_internal.h's
// doc comment on bb_data_http_events_route() for the schema-only rationale.
// ---------------------------------------------------------------------------

static const bb_route_param_t s_events_params[] = {
    { .name = "topic", .in = "query",
      .description = "Optional topic filter (see bb_data_http_attach()'s topic "
                      "argument); omit to receive every attached topic multiplexed "
                      "on one stream.",
      .required = false, .schema_type = "string" },
};

static const bb_route_response_t s_events_responses[] = {
    { .status = 200, .content_type = "text/event-stream", .schema = NULL,
      .description = "Multiplexed SSE stream over every bb_data-bound topic "
                      "attached via bb_data_http_attach(); see the registered "
                      "SSE topic schemas' oneOf below for the per-frame shape." },
    { .status = 0 },
};

static const bb_route_t s_events_route = {
    .method           = BB_HTTP_GET,
    .path             = "/api/events",
    .tag              = "data",
    .summary          = "Server-Sent Events stream, multiplexed over every "
                        "attached bb_data topic",
    .responses        = s_events_responses,
    .handler          = NULL,  // schema-only -- see bb_data_http_events_route()'s doc
    .parameters       = s_events_params,
    .parameters_count = 1,
};

const bb_route_t *bb_data_http_events_route(void)
{
    return &s_events_route;
}
