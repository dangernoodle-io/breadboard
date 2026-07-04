#include "bb_event_routes.h"
#ifndef ARDUINO
#include "bb_arena.h"
#endif
#include "bb_event.h"
#include "bb_event_ring.h"
#include "bb_log.h"
#include "bb_event_routes_internal.h"
#include "bb_event_topic_registry.h"
#include "bb_event_routes_defaults.h"

#include <assert.h>
#ifndef ARDUINO
#include <pthread.h>
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*bb_event_routes_calloc_fn)(size_t n, size_t sz);
typedef void  (*bb_event_routes_free_fn)(void *p);
static bb_event_routes_calloc_fn s_calloc = calloc;
static bb_event_routes_free_fn   s_free   = free;

void bb_event_routes_set_allocator(bb_event_routes_calloc_fn c, bb_event_routes_free_fn f) {
    s_calloc = c ? c : calloc;
    s_free   = f ? f : free;
}
void bb_event_routes_reset_allocator(void) {
    s_calloc = calloc;
    s_free   = free;
}

static const char *TAG = "bb_event_routes";

#ifndef CONFIG_BB_EVENT_ROUTES_RING_CAPACITY
#define CONFIG_BB_EVENT_ROUTES_RING_CAPACITY 8
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_RETAINED_RING_CAPACITY
#define CONFIG_BB_EVENT_ROUTES_RETAINED_RING_CAPACITY 1
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY
#define CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY 256
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_HEARTBEAT_MS
#define CONFIG_BB_EVENT_ROUTES_HEARTBEAT_MS 15000
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_MAX_TOPICS
#define CONFIG_BB_EVENT_ROUTES_MAX_TOPICS 8
#endif

#define TOPIC_NAME_MAX BB_EVENT_TOPIC_NAME_MAX

// ---------------------------------------------------------------------------
// Attached topics
//
// Storage is a fixed array, indexed identically to bb_event_topic_registry's
// registration order (register-only, no deregister — see
// bb_event_topic_registry.h). The registry (a thin consumer of the generic
// bb_registry primitive on host/ESP-IDF; a plain array scan on Arduino)
// replaces the old manual dedupe-scan + lookup-by-handle bookkeeping.
// ---------------------------------------------------------------------------

typedef bb_event_attached_topic_t attached_topic_t;

static attached_topic_t s_topics[CONFIG_BB_EVENT_ROUTES_MAX_TOPICS];
static void *s_topics_lock = NULL;  // guards the compound attach_ex2 sequence below

// ---------------------------------------------------------------------------
// Client slots
// ---------------------------------------------------------------------------

typedef struct queue_entry {
    int topic_idx;       // index into s_topics
    int32_t event_id;    // bb_event id
    size_t size;
    uint8_t *payload;    // borrowed; allocated alongside slot
} queue_entry_t;

struct bb_event_routes_client {
    atomic_bool in_use;

    // Ring queue
    queue_entry_t *entries;       // [queue_depth]
    uint8_t *payload_buf;         // [queue_depth * ring_max_entry]
    size_t queue_depth;
    size_t max_entry;
    size_t head;                  // write
    size_t tail;                  // read
    size_t count;
    uint64_t dropped;             // SSE id jumps reflect this
    uint64_t next_emit_id;        // monotonic id assigned at frame emit

    // One subscription per attached topic; entries[i] valid iff i<s_num_topics_snapshot.
    bb_event_sub_t subs[CONFIG_BB_EVENT_ROUTES_MAX_TOPICS];
    size_t num_subs;

    void *port_lock;              // platform-supplied mutex (opaque)
    void *event;                  // platform-supplied signal (opaque) — wakes SSE writer
};

static bb_event_routes_client_t s_clients[CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS];

// The index-addressed bb_arena pool below (SSE_pool) needs bb_arena/pthread,
// neither of which is on the Arduino build's include path — /api/events is
// a 503 stub there (CC3000 RAM constraints; see breadboard CLAUDE.md), so
// the pool is compiled out and bb_event_routes_client_acquire_ex falls back
// to the pre-pool per-client s_calloc allocation (see the #else branches
// below) purely to keep this shared file compiling on that backend.
#ifndef ARDUINO

// ---------------------------------------------------------------------------
// SSE pool (B1-478 PR E, heap-backed-by-default per B1-491)
//
// Pre-allocates a per-client bundle (queue entry array + payload buffer) as
// a fixed array carved once from an SSE-private bb_arena. Client slot i
// (the index the existing CAS client-slot loop in
// bb_event_routes_client_acquire_ex assigns atomically) deterministically
// owns s_sse_bundles[i] for the arena's lifetime — no acquire/release, no
// free-list, no shared mutable pool state, no per-connect malloc/free churn.
//
// CONFIG_BB_EVENT_ROUTES_POOL_STATIC selects the arena's *backing*, not
// whether pooling happens:
//   n (default) — sse_pool_ensure() lazily creates a heap arena on the FIRST
//     client acquire (bb_event_routes_client_acquire_ex), via the same
//     (possibly SPIRAM-preferred / test-injected) allocator used elsewhere
//     in this file. Never freed once created (see sse_pool_ensure() doc).
//   y — the arena is carved eagerly from a permanent static-BSS buffer at
//     bb_event_routes_init() time (unchanged PR E behavior).
// ---------------------------------------------------------------------------

typedef struct {
    queue_entry_t entries[CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH];
    uint8_t payload[CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH * CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY];
} sse_bundle_t;

// Documented flat allowance covering bb_arena's internal header struct
// (private to bb_arena.c — not visible here) plus max_align_t rounding of
// the single bb_arena_alloc() call below. Not a magic constant: the bulk of
// the buffer is computed from MAX_CLIENTS * sizeof(sse_bundle_t); this is
// only the small fixed overhead on top of that.
#define SSE_ARENA_HDR_ALLOWANCE_BYTES 128u
#define SSE_BUNDLES_BYTES \
    ((size_t)CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS * sizeof(sse_bundle_t))

// Guards against size_t overflow in the multiplication above so the
// buffer-size computation below is trustworthy at compile time.
_Static_assert(CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS == 0 ||
               (SSE_BUNDLES_BYTES / CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS) == sizeof(sse_bundle_t),
               "SSE bundle byte count overflowed size_t");

// Static-BSS backing buffer — only used for the POOL_STATIC=y path. Also
// compiled under BB_EVENT_ROUTES_TESTING so the host suite can flip between
// the static and lazy-heap backings in the same binary via
// bb_event_routes_set_static_pool_for_test().
#if CONFIG_BB_EVENT_ROUTES_POOL_STATIC || defined(BB_EVENT_ROUTES_TESTING)
static uint8_t s_sse_arena_buf[
    SSE_BUNDLES_BYTES + SSE_ARENA_HDR_ALLOWANCE_BYTES
] __attribute__((aligned(_Alignof(max_align_t))));
#endif

static bb_arena_t    s_sse_arena;      // NULL until sse_pool_ensure() creates it
static sse_bundle_t *s_sse_bundles;    // [MAX_CLIENTS], index == client slot; NULL until created
static bool          s_sse_pool_is_static;  // backing actually used by s_sse_arena (mirrors bb_pub's *_is_static flag)

// Dedicated leaf mutex guarding ONLY the sse_pool_ensure() first-alloc
// critical section below (portable POSIX pthread, host + ESP-IDF — mirrors
// bb_arena_tls's s_arena_mtx pattern). esp_http_server dispatches requests
// on a single task today, so the unguarded check-then-allocate-then-publish
// sequence was safe in practice, but this exact TOCTOU shape has shipped
// twice before on this component (PR E CRITICALs) — harden rather than
// document. Never taken while holding any other bb_event_routes lock
// (s_topics_lock / per-client port_lock): sse_pool_ensure() is called only
// from bb_event_routes_client_acquire_ex() before any other lock in this
// file is acquired, and from bb_event_routes_init()/register_routes_init()
// before init completes — so this mutex is always the innermost (and only)
// lock held during its critical section. Keep it that way: do not call
// anything from inside the locked section that could re-enter
// sse_pool_ensure() or take s_topics_lock.
static pthread_mutex_t s_sse_pool_mtx = PTHREAD_MUTEX_INITIALIZER;

#ifdef BB_EVENT_ROUTES_TESTING
// Runtime flag that lets the host test suite exercise the static-pool path
// without requiring a compile-time Kconfig change. Allows both dynamic and
// static paths to be tested in the same test binary.
static bool s_use_static_pool_for_test = false;
void bb_event_routes_set_static_pool_for_test(bool use_static) {
    s_use_static_pool_for_test = use_static;
}

// Test accessor: true once sse_pool_ensure() has created the pool (either
// backing). Used to assert the lazy-heap pool stays uncreated until the
// first client acquire.
bool bb_event_routes_pool_created_for_test(void) {
    return s_sse_bundles != NULL;
}
#endif

// Returns true when the static-pool path is active (either via Kconfig or
// the test override).
static bool use_static_pool(void) {
#if CONFIG_BB_EVENT_ROUTES_POOL_STATIC
    return true;
#elif defined(BB_EVENT_ROUTES_TESTING)
    return s_use_static_pool_for_test;
#else
    return false;
#endif
}

// Create the SSE bundle pool on demand. Idempotent — returns BB_OK
// immediately once s_sse_bundles is set (either backing).
//
// STATIC path: the backing buffer is compile-time sized to always fit (see
// the static asserts above), so both calls are provably unreachable-on-
// failure — assert() rather than a branch + return, so there is no
// uncovered error branch to test. Called eagerly from bb_event_routes_init
// (unchanged PR E behavior — deterministic pool from boot).
//
// Lazy-heap path (default): called on the FIRST client acquire
// (bb_event_routes_client_acquire_ex). Allocates one contiguous block via
// the current allocator (s_calloc — SPIRAM-preferred on ESP-IDF via
// bb_event_routes_spiram_init, test-injectable via
// bb_event_routes_set_allocator) and carves the arena from it. A failed
// allocation (fragmented/low heap) fails soft: returns BB_ERR_NO_SPACE, the
// same code the caller already returns for pool exhaustion, so the
// connection is cleanly rejected and retried on the next connect — no
// crash, no unchecked NULL. The block is never freed once allocated (no
// idle-reclaim of this arena — see the CONFIG_BB_EVENT_ROUTES_POOL_STATIC
// Kconfig help and the breadboard CLAUDE.md for the UAF-safety argument);
// one standing allocation for the process lifetime, not a permanent BSS
// reservation.
static bb_err_t sse_pool_ensure(void)
{
    pthread_mutex_lock(&s_sse_pool_mtx);

    if (s_sse_bundles) {
        pthread_mutex_unlock(&s_sse_pool_mtx);
        return BB_OK;
    }

    if (use_static_pool()) {
#if CONFIG_BB_EVENT_ROUTES_POOL_STATIC || defined(BB_EVENT_ROUTES_TESTING)
        bb_err_t arena_err = bb_arena_init(&s_sse_arena, s_sse_arena_buf, sizeof(s_sse_arena_buf));
        assert(arena_err == BB_OK);  // LCOV_EXCL_BR_LINE — buffer is compile-time sized to fit, see static asserts above
        (void)arena_err;  // avoid unused-variable warning when NDEBUG compiles the assert out
        s_sse_bundles = (sse_bundle_t *)bb_arena_alloc(s_sse_arena, SSE_BUNDLES_BYTES);
        assert(s_sse_bundles != NULL);  // LCOV_EXCL_BR_LINE — arena sized generously above, see static asserts above
        s_sse_pool_is_static = true;
        pthread_mutex_unlock(&s_sse_pool_mtx);
        return BB_OK;
#else
        pthread_mutex_unlock(&s_sse_pool_mtx);  // LCOV_EXCL_LINE — unreachable: use_static_pool() only true when the branch above compiles
        return BB_ERR_INVALID_STATE;  // LCOV_EXCL_LINE — unreachable: use_static_pool() only true when the branch above compiles
#endif
    }

    size_t total = SSE_BUNDLES_BYTES + SSE_ARENA_HDR_ALLOWANCE_BYTES;
    void *block = s_calloc(1, total);
    if (!block) {
        bb_log_w(TAG, "SSE pool: heap alloc failed (%zu bytes); connection rejected, retrying next connect", total);
        pthread_mutex_unlock(&s_sse_pool_mtx);
        return BB_ERR_NO_SPACE;
    }
    bb_err_t err = bb_arena_init(&s_sse_arena, block, total);
    if (err != BB_OK) {  // LCOV_EXCL_BR_LINE — total is compile-time sized to fit (same computation as the static path above)
        // LCOV_EXCL_START
        s_free(block);
        pthread_mutex_unlock(&s_sse_pool_mtx);
        return BB_ERR_NO_SPACE;
        // LCOV_EXCL_STOP
    }
    s_sse_bundles = (sse_bundle_t *)bb_arena_alloc(s_sse_arena, SSE_BUNDLES_BYTES);
    if (!s_sse_bundles) {  // LCOV_EXCL_BR_LINE — arena sized exactly for this one alloc above
        // LCOV_EXCL_START
        s_free(block);
        s_sse_arena = NULL;
        pthread_mutex_unlock(&s_sse_pool_mtx);
        return BB_ERR_NO_SPACE;
        // LCOV_EXCL_STOP
    }
    s_sse_pool_is_static = false;
    pthread_mutex_unlock(&s_sse_pool_mtx);
    return BB_OK;
}

#endif // !ARDUINO — SSE pool (bb_arena/pthread)

// ---------------------------------------------------------------------------
// Config (resolved at init)
// ---------------------------------------------------------------------------

static struct {
    size_t max_clients;
    size_t queue_depth;
    size_t ring_capacity;
    size_t ring_max_entry;
    uint32_t heartbeat_ms;
    bool initialized;
} s_cfg;

// ---------------------------------------------------------------------------
// Capture: bb_event subscriber callback. Enqueues to a client slot.
// ---------------------------------------------------------------------------

static void capture_cb(bb_event_topic_t topic, int32_t id,
                       const void *data, size_t size, void *user)
{
    bb_event_routes_client_t *c = (bb_event_routes_client_t *)user;
    if (!c || !atomic_load(&c->in_use)) return;  // LCOV_EXCL_BR_LINE — defensive

    // Locate topic index via the registry. bb_event_topic_registry_register
    // is only ever called (in attach_ex2) after the entry's topic/ring/name
    // fields are fully populated, and the registry's own internal lock
    // (host/ESP-IDF) establishes happens-before for that write — no need to
    // also hold s_topics_lock here.
    size_t topic_idx = 0;
    if (bb_event_topic_registry_find_by_handle(topic, &topic_idx) != BB_OK) {  // LCOV_EXCL_BR_LINE — attach guarantees a match
        return;  // LCOV_EXCL_LINE
    }

    size_t copy = size > c->max_entry ? c->max_entry : size;

    bb_event_routes_port_lock(c->port_lock);

    if (c->count == c->queue_depth) {
        // Drop oldest
        c->tail = (c->tail + 1) % c->queue_depth;
        c->count--;
        c->dropped++;
    }

    size_t w = c->head;
    queue_entry_t *e = &c->entries[w];
    e->topic_idx = (int)topic_idx;
    e->event_id = id;
    e->size = copy;
    e->payload = c->payload_buf + (w * c->max_entry);
    if (copy > 0 && data) {  // LCOV_EXCL_BR_LINE — dispatch supplies valid data
        memcpy(e->payload, data, copy);
    }

    c->head = (c->head + 1) % c->queue_depth;
    c->count++;

    bb_event_routes_port_unlock(c->port_lock);

    bb_event_routes_port_event_signal(c->event);
}

// ---------------------------------------------------------------------------
// Public init / attach
// ---------------------------------------------------------------------------

bb_err_t bb_event_routes_init(const bb_event_routes_cfg_t *cfg)
{
    if (s_cfg.initialized) return BB_OK;

    s_topics_lock = bb_event_routes_port_lock_create();
    if (!s_topics_lock) return BB_ERR_NO_SPACE;  // LCOV_EXCL_LINE — OOM on lock alloc

    s_cfg.max_clients    = (cfg && cfg->max_clients)        ? cfg->max_clients        : CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS;
    s_cfg.queue_depth    = (cfg && cfg->per_client_queue)   ? cfg->per_client_queue   : CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH;
    s_cfg.ring_capacity  = (cfg && cfg->ring_capacity)      ? cfg->ring_capacity      : CONFIG_BB_EVENT_ROUTES_RING_CAPACITY;
    s_cfg.ring_max_entry = (cfg && cfg->ring_max_entry)     ? cfg->ring_max_entry     : CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY;
    s_cfg.heartbeat_ms   = (cfg && cfg->heartbeat_ms)       ? cfg->heartbeat_ms       : CONFIG_BB_EVENT_ROUTES_HEARTBEAT_MS;

    if (s_cfg.max_clients > CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS) {
        return BB_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS; i++) {
        atomic_store(&s_clients[i].in_use, false);
    }

#ifndef ARDUINO
    // STATIC backing is eager (deterministic pool from boot, unchanged PR E
    // behavior); the lazy-heap default backing is created later, on the
    // first client acquire (see sse_pool_ensure()).
    if (use_static_pool()) {
        bb_err_t pool_err = sse_pool_ensure();
        assert(pool_err == BB_OK);  // LCOV_EXCL_BR_LINE — static backing is provably unreachable-on-failure, see sse_pool_ensure()
        (void)pool_err;  // avoid unused-variable warning when NDEBUG compiles the assert out
    }
#endif // !ARDUINO

    s_cfg.initialized = true;
    bb_log_i(TAG, "initialized: max_clients=%zu queue_depth=%zu ring=%zu/%zu hb=%ums",
             s_cfg.max_clients, s_cfg.queue_depth, s_cfg.ring_capacity,
             s_cfg.ring_max_entry, (unsigned)s_cfg.heartbeat_ms);
    return BB_OK;
}

bb_err_t bb_event_routes_attach_ex2(const char *topic_name, bool retained,
                                    size_t max_entry)
{
    if (!topic_name) return BB_ERR_INVALID_ARG;
    if (!s_cfg.initialized) return BB_ERR_INVALID_STATE;

    // Hold the topics lock for the full attach: dedupe + bounds check + ring
    // ops + commit.  bb_event_ring_attach_ex uses its own bb_event_lock (a
    // separate lock from s_topics_lock) so there is no deadlock risk.
    // Holding the lock across the whole operation prevents TOCTOU without
    // requiring a re-check after re-acquire. (The registry also serialises
    // its own ops internally on host/ESP-IDF, but that alone would not be
    // enough here: the compound reserve-slot + ring-create + populate +
    // commit sequence below must run as a single atomic unit, which only
    // s_topics_lock — held across the whole function — guarantees.)
    bb_event_routes_port_lock(s_topics_lock);

    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_lookup(topic_name, &topic);
    if (err != BB_OK) {
        bb_event_routes_port_unlock(s_topics_lock);
        bb_log_e(TAG, "topic '%s' not registered", topic_name);
        return err;
    }

    size_t existing_idx;
    if (bb_event_topic_registry_find_by_handle(topic, &existing_idx) == BB_OK) {
        bb_event_routes_port_unlock(s_topics_lock);
        return BB_OK;  // already attached — idempotent
    }

    if (bb_event_topic_registry_count() >= CONFIG_BB_EVENT_ROUTES_MAX_TOPICS) {
        bb_event_routes_port_unlock(s_topics_lock);
        bb_log_e(TAG, "topic table full");
        return BB_ERR_NO_SPACE;
    }

    size_t ring_cap = retained
        ? CONFIG_BB_EVENT_ROUTES_RETAINED_RING_CAPACITY
        : s_cfg.ring_capacity;
    size_t entry_size = max_entry ? max_entry : s_cfg.ring_max_entry;
    bb_event_ring_t ring = NULL;
    err = bb_event_ring_attach_ex(topic, ring_cap, entry_size, retained, &ring);
    if (err != BB_OK) {
        bb_event_routes_port_unlock(s_topics_lock);
        bb_log_e(TAG, "ring attach failed for '%s': %d", topic_name, err);
        return err;
    }

    // Next free slot: safe to compute from the registry's count because
    // s_topics_lock (held for this whole function) serialises every
    // attach_ex2 call, and the registry is register-only (no deregister),
    // so its count always equals the number of slots already committed.
    attached_topic_t *t = &s_topics[bb_event_topic_registry_count()];
    strncpy(t->name, topic_name, TOPIC_NAME_MAX - 1);
    t->name[TOPIC_NAME_MAX - 1] = '\0';
    t->topic = topic;
    t->ring = ring;

    err = bb_event_topic_registry_register(t->name, t);
    if (err != BB_OK) {  // LCOV_EXCL_BR_LINE — count/dedupe checks above (find_by_handle miss + count < cap) make this unreachable in practice.
        // LCOV_EXCL_START
        bb_event_routes_port_unlock(s_topics_lock);
        bb_log_e(TAG, "registry register failed for '%s': %d", topic_name, err);
        return err;
        // LCOV_EXCL_STOP
    }
    bb_event_routes_port_unlock(s_topics_lock);

    bb_log_d(TAG, "attached '%s' retained=%d max_entry=%zu", topic_name,
             (int)retained, entry_size);
    return BB_OK;
}

bb_err_t bb_event_routes_attach_ex(const char *topic_name, bool retained)
{
    return bb_event_routes_attach_ex2(topic_name, retained, 0);
}

bb_err_t bb_event_routes_attach(const char *topic_name)
{
    return bb_event_routes_attach_ex(topic_name, false);
}

// ---------------------------------------------------------------------------
// Client lifecycle (called by route handler)
// ---------------------------------------------------------------------------

bb_err_t bb_event_routes_client_acquire_ex(bb_event_routes_client_t **out,
                                           const char *topic_filter)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (!s_cfg.initialized) return BB_ERR_INVALID_STATE;

#ifndef ARDUINO
    // Lazy-heap default: create the pool on the first-ever acquire. No-op
    // once created (either backing) — see sse_pool_ensure(). A failed heap
    // allocation here fails soft, exactly like the pool-exhaustion return
    // below: the connection is rejected and retried on the next connect.
    bb_err_t pool_err = sse_pool_ensure();
    if (pool_err != BB_OK) return pool_err;
#endif

    for (size_t i = 0; i < s_cfg.max_clients; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&s_clients[i].in_use, &expected, true)) {
            bb_event_routes_client_t *c = &s_clients[i];
            c->queue_depth = s_cfg.queue_depth;
            c->max_entry = s_cfg.ring_max_entry;
            c->head = c->tail = c->count = 0;
            c->dropped = 0;
            c->next_emit_id = 1;
            c->num_subs = 0;

            // Validate that runtime cfg fits within the compile-time pool dimensions.
            if (c->queue_depth > CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH ||
                c->max_entry > CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY) {
                atomic_store(&c->in_use, false);
                return BB_ERR_NO_SPACE;
            }
#ifndef ARDUINO
            // Index-addressed: client slot i (this loop's index)
            // deterministically owns s_sse_bundles[i] for the pool's
            // lifetime. The CAS claim above already serialises ownership
            // of index i — no acquire/release, no free-list, no shared
            // mutable pool state. i < s_cfg.max_clients <= MAX_CLIENTS
            // (checked at init), so the index is always valid.
            sse_bundle_t *bundle = &s_sse_bundles[i];
            c->entries = bundle->entries;
            c->payload_buf = bundle->payload;
            memset(c->entries, 0, c->queue_depth * sizeof(queue_entry_t));
            memset(c->payload_buf, 0, c->queue_depth * c->max_entry);
#else
            // No SSE pool on Arduino (see the #ifndef ARDUINO guard around
            // the pool machinery above) — fall back to the pre-pool
            // per-client heap allocation. Unreachable in practice: /api/events
            // is a 503 stub on Arduino, so no route handler ever calls this.
            c->entries = (queue_entry_t *)s_calloc(c->queue_depth, sizeof(queue_entry_t));
            c->payload_buf = (uint8_t *)s_calloc(c->queue_depth, c->max_entry);
#endif

            c->port_lock = bb_event_routes_port_lock_create();
            c->event = bb_event_routes_port_event_create();
#ifndef ARDUINO
            // port_lock/event allocate via a raw platform malloc (pthread
            // mutex on host), not the overridable s_calloc/bb_mem facades
            // used elsewhere in this file — there is no allocator-injection
            // seam for this path, so it is unreachable in the host test
            // suite (real OOM only). entries/payload_buf are index-owned
            // pool bundles and can never be NULL here.
            if (!c->port_lock || !c->event) {  // LCOV_EXCL_BR_LINE — see comment above
                // LCOV_EXCL_START
                if (c->port_lock) bb_event_routes_port_lock_destroy(c->port_lock);
                if (c->event) bb_event_routes_port_event_destroy(c->event);
                c->entries = NULL;
                c->payload_buf = NULL;
                c->port_lock = NULL;
                c->event = NULL;
                atomic_store(&c->in_use, false);
                return BB_ERR_NO_SPACE;
                // LCOV_EXCL_STOP
#else
            if (!c->entries || !c->payload_buf || !c->port_lock || !c->event) {
                s_free(c->entries);
                s_free(c->payload_buf);
                if (c->port_lock) bb_event_routes_port_lock_destroy(c->port_lock);
                if (c->event) bb_event_routes_port_event_destroy(c->event);
                c->entries = NULL;
                c->payload_buf = NULL;
                c->port_lock = NULL;
                c->event = NULL;
                atomic_store(&c->in_use, false);
                return BB_ERR_NO_SPACE;
#endif
            }

            // Snapshot the topic table under the lock so concurrent attach_ex
            // calls cannot race the iteration below.
            typedef struct { bb_event_ring_t ring; char name[TOPIC_NAME_MAX]; } topic_snap_t;
            topic_snap_t snaps[CONFIG_BB_EVENT_ROUTES_MAX_TOPICS];
            size_t snap_count;
            bb_event_routes_port_lock(s_topics_lock);
            snap_count = bb_event_topic_registry_count();
            for (size_t k = 0; k < snap_count; k++) {
                snaps[k].ring = s_topics[k].ring;
                memcpy(snaps[k].name, s_topics[k].name, TOPIC_NAME_MAX);
            }
            bb_event_routes_port_unlock(s_topics_lock);

            // Subscribe to matching topics. Replay-on-connect via ring.
            for (size_t k = 0; k < snap_count; k++) {
                // Filter to topic_filter if specified.
                if (topic_filter && strcmp(snaps[k].name, topic_filter) != 0) {
                    continue;
                }
                bb_event_sub_t sub = NULL;
                bb_err_t err = bb_event_ring_subscribe_with_replay(
                    snaps[k].ring, capture_cb, c, &sub);
                if (err != BB_OK) {
                    // Roll back: unsubscribe what we already did, release.
                    // Pool bundles (entries/payload_buf) are index-owned —
                    // nothing to free there.
                    for (size_t j = 0; j < c->num_subs; j++) {
                        bb_event_unsubscribe(c->subs[j]);
                    }
                    bb_event_routes_port_lock_destroy(c->port_lock);
                    bb_event_routes_port_event_destroy(c->event);
                    c->entries = NULL;
                    c->payload_buf = NULL;
                    c->port_lock = NULL;
                    c->event = NULL;
                    atomic_store(&c->in_use, false);
                    return err;
                }
                c->subs[c->num_subs++] = sub;
            }

            *out = c;
            return BB_OK;
        }
    }
    return BB_ERR_NO_SPACE;
}

bb_err_t bb_event_routes_client_acquire(bb_event_routes_client_t **out)
{
    return bb_event_routes_client_acquire_ex(out, NULL);
}

void bb_event_routes_client_release(bb_event_routes_client_t *c)
{
    if (!c) return;
    for (size_t i = 0; i < c->num_subs; i++) {
        bb_event_unsubscribe(c->subs[i]);
    }
    c->num_subs = 0;
#ifndef ARDUINO
    // Index-owned pool bundles (entries/payload_buf) are never released
    // here — the client slot (freed just below via in_use=false) is what
    // makes slot i reusable, and the next acquire on that slot
    // deterministically gets the same s_sse_bundles[i], regardless of
    // backing (static or lazy-heap).
#else
    // Arduino fallback path allocates per-client (no SSE pool available;
    // see the #ifndef ARDUINO guard around the pool machinery above).
    s_free(c->entries);
    s_free(c->payload_buf);
#endif
    if (c->port_lock) bb_event_routes_port_lock_destroy(c->port_lock);  // LCOV_EXCL_BR_LINE — port_lock always set on release path
    if (c->event) bb_event_routes_port_event_destroy(c->event);  // LCOV_EXCL_BR_LINE — event always set on release path
    c->entries = NULL;
    c->payload_buf = NULL;
    c->port_lock = NULL;
    c->event = NULL;
    atomic_store(&c->in_use, false);
}

// ---------------------------------------------------------------------------
// Frame builder (drains one entry; caller handles transport)
// ---------------------------------------------------------------------------

size_t bb_event_routes_drain_frame(bb_event_routes_client_t *c, char *buf, size_t buflen)
{
    if (!c || !buf || buflen < 32) return 0;

    bb_event_routes_port_lock(c->port_lock);
    if (c->count == 0) {
        bb_event_routes_port_unlock(c->port_lock);
        return 0;
    }
    queue_entry_t e = c->entries[c->tail];
    // Copy payload locally so we can release the lock before serializing.
    uint8_t local_payload[CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY];
    // e.size is already capped at c->max_entry <= ring_max_entry; cap defensively.
    size_t copy = e.size > sizeof(local_payload) ? sizeof(local_payload) : e.size;  // LCOV_EXCL_BR_LINE
    if (copy > 0) memcpy(local_payload, e.payload, copy);
    c->tail = (c->tail + 1) % c->queue_depth;
    c->count--;
    uint64_t emit_id = c->next_emit_id + c->dropped;
    c->next_emit_id = emit_id + 1;
    uint64_t carried_drops = c->dropped;
    c->dropped = 0;
    bb_event_routes_port_unlock(c->port_lock);

    // Read topic name under the topics lock; copy to local so serialization
    // is lock-free.  capture_cb guarantees topic_idx is always valid.
    char topic_name[TOPIC_NAME_MAX];
    bb_event_routes_port_lock(s_topics_lock);
    memcpy(topic_name, s_topics[e.topic_idx].name, TOPIC_NAME_MAX);
    bb_event_routes_port_unlock(s_topics_lock);
    int n;
    if (copy == 0) {
        n = snprintf(buf, buflen, "event: %s\ndata: {}\nid: %llu\n\n",
                     topic_name, (unsigned long long)emit_id);
    } else {
        n = snprintf(buf, buflen, "event: %s\ndata: %.*s\nid: %llu\n\n",
                     topic_name, (int)copy, (const char *)local_payload,
                     (unsigned long long)emit_id);
    }
    if (n < 0 || (size_t)n >= buflen) {  // LCOV_EXCL_BR_LINE — n<0 unreachable for printf format
        // Truncated; emit a safe fallback so the SSE frame is still valid.
        n = snprintf(buf, buflen, "event: %s\ndata: {\"truncated\":true}\nid: %llu\n\n",
                     topic_name, (unsigned long long)emit_id);
    }
    (void)carried_drops;  // visible via /api/events status endpoint in a future iteration
    return (size_t)(n > 0 ? n : 0);  // LCOV_EXCL_BR_LINE — fallback frame always positive
}

uint32_t bb_event_routes_heartbeat_ms(void) { return s_cfg.heartbeat_ms; }

void *bb_event_routes_client_event(bb_event_routes_client_t *c)
{
    return c ? c->event : NULL;
}

int bb_event_routes_client_slot_index(const bb_event_routes_client_t *c)
{
    if (!c) return -1;
    ptrdiff_t idx = c - s_clients;
    if (idx < 0 || (size_t)idx >= CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS) return -1;
    return (int)idx;
}

// ---------------------------------------------------------------------------
// Diagnostics accessors (public, no ESP-IDF deps)
// ---------------------------------------------------------------------------

size_t bb_event_routes_topic_count(void)
{
    return bb_event_topic_registry_count();
}

bb_err_t bb_event_routes_topic_info(size_t idx,
                                    const char **name,
                                    bb_event_ring_t *ring)
{
    attached_topic_t *t = NULL;
    bb_err_t err = bb_event_topic_registry_get_by_index(idx, &t);
    if (err != BB_OK) {
        return err;
    }
    if (name) *name = t->name;
    if (ring) *ring = t->ring;
    return BB_OK;
}

size_t bb_event_routes_active_client_count(void)
{
    size_t active = 0;
    for (size_t i = 0; i < CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS; i++) {
        if (atomic_load(&s_clients[i].in_use)) active++;
    }
    return active;
}

// B1-492: reuse-deferred 503 counter. Incremented only from the ESP-IDF
// platform component's events_handler() (single httpd task — no concurrent
// writers), read from anywhere via the public getter.
static size_t s_slot_reuse_deferred_count;

void bb_event_routes_note_slot_reuse_deferred(void)
{
    s_slot_reuse_deferred_count++;
}

size_t bb_event_routes_slot_reuse_deferred_count(void)
{
    return s_slot_reuse_deferred_count;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_EVENT_ROUTES_TESTING
void bb_event_routes_reset_for_test(void) {
    for (size_t i = 0; i < CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS; i++) {
        if (atomic_load(&s_clients[i].in_use)) {
            bb_event_routes_client_release(&s_clients[i]);
        }
    }
    size_t num_topics = bb_event_topic_registry_count();
    for (size_t i = 0; i < num_topics; i++) {
        attached_topic_t *t = NULL;
        if (bb_event_topic_registry_get_by_index(i, &t) == BB_OK && t->ring) {  // LCOV_EXCL_BR_LINE
            bb_event_ring_detach(t->ring);
        }
    }
    bb_event_topic_registry_test_reset();
    memset(&s_cfg, 0, sizeof(s_cfg));
    if (s_topics_lock) {
        bb_event_routes_port_lock_destroy(s_topics_lock);
        s_topics_lock = NULL;
    }
    // Tear down the SSE pool so the next test's chosen backing (static vs
    // lazy-heap, via bb_event_routes_set_static_pool_for_test) takes effect
    // cleanly, and so lazy-creation-on-first-acquire can be re-exercised.
    // s_sse_arena and the heap block s_calloc returned are the same pointer
    // (bb_arena carves its header from the front of the caller-supplied
    // block) — freeing s_sse_arena directly is correct, not a mismatch.
    if (s_sse_arena && !s_sse_pool_is_static) {
        s_free((void *)s_sse_arena);
    }
    s_sse_arena = NULL;
    s_sse_bundles = NULL;
    s_sse_pool_is_static = false;
    s_use_static_pool_for_test = false;
    s_slot_reuse_deferred_count = 0;
}

size_t bb_event_routes_queued_for_test(bb_event_routes_client_t *c) {
    return c ? c->count : 0;  // LCOV_EXCL_BR_LINE
}

uint64_t bb_event_routes_dropped_for_test(bb_event_routes_client_t *c) {
    return c ? c->dropped : 0;  // LCOV_EXCL_BR_LINE
}
#endif
