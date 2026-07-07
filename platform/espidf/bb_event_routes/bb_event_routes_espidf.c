// ESP-IDF route handler for bb_event_routes — registers GET /api/events,
// spawns a FreeRTOS task per client, drains queued events to SSE frames.
// Also registers GET /api/diag/events for topic discovery + ring diagnostics.
#include "bb_event_routes.h"
#include "bb_event_routes_internal.h"
#include "bb_event_ring.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "bb_init.h"
#include "bb_sse_writer.h"
#include "bb_timer.h"
#include "bb_task.h"
#include "bb_mem_arena.h"
#include "bb_pool.h"
#include "sse_bundle_decision.h"
#include "sse_pool_reclaim_decision.h"
#include "sse_connect_error_decision.h"

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bb_event_routes";

// ---------------------------------------------------------------------------
// SSE writer task-bundle pool (B1-478 PR E baseline; B1-484/B1-492 fix)
//
// Per-client bundle {stack, TCB} for xTaskCreateStatic, drawn from a
// bb_pool_t in SLOTS mode (an ESP-IDF-only pool — StackType_t/StaticTask_t
// are FreeRTOS types and cannot appear in the portable, host-compiled
// bb_event_routes_common.c). Unlike the pre-B1-492 shape, a client's task
// bundle is NOT index-addressed to that client's s_clients[] slot: it is an
// independent bb_pool_acquire()/bb_pool_release() lease, decoupled from
// bb_event_routes_client_acquire_ex()'s CAS slot claim.
//
// B1-484: xTaskCreateStatic() re-initializes a bundle's stack/TCB in place.
// The exiting SSE task that most recently owned a bundle does not finish
// tearing down synchronously with the HTTP handler that frees its client
// slot — reissuing that bundle's stack/TCB to a brand-new task before the
// old task has actually stopped running on it corrupts a live stack/TCB.
// bb_pool's optional SLOTS callbacks (B1-479) close this gap:
//   - slot_reusable (sse_bundle_reusable): single non-blocking
//     eTaskGetState(handle)==eSuspended check — no loop, no timeout. A
//     released-but-still-running task's bundle is simply not reissued yet.
//   - slot_reap (sse_bundle_reap): external vTaskDelete(handle) the moment
//     slot_reusable first confirms eSuspended, immediately before reissue.
//   - on_acquire (sse_bundle_on_acquire): lazily creates the bundle's
//     completion semaphore on its first-ever acquire (relies on bb_pool
//     zero-initializing SLOTS storage so `done_sem == NULL` reliably means
//     "never created" only once, not on every reacquire).
// The exiting task itself (see sse_task_done, "Exiting-task tail" below)
// NEVER self-deletes: as its absolute last act it gives the bundle's
// completion semaphore and calls vTaskSuspend(NULL), leaving a suspended
// "corpse" occupying its static slot — safe to poll (eTaskGetState) or
// externally vTaskDelete indefinitely, since a statically/arena-backed
// TCB is never returned to a general allocator.
//
// bb_event_routes_espidf.c's events_handler() acquire path
// (bb_pool_acquire) never blocks: if the prior occupant's task hasn't yet
// reached eSuspended, acquire returns NULL and events_handler fast-rejects
// with the existing 503 max_clients path (EventSource auto-retries) — no
// vTaskDelay, no loop, no deadline, no permanently stranded slot (contrast
// with the abandoned jae/bb-sse-reaper design, which gated reuse on a fixed
// timeout and could permanently strand every slot under boot contention).
//
// CONFIG_BB_EVENT_ROUTES_POOL_STATIC selects the pool's backing arena (see
// the matching comment in bb_event_routes_common.c):
//   n (default) — sse_task_bundles_ensure() lazily creates a heap-backed
//     pool (bb_pool_create_owned, BB_POOL_BACKING_HEAP; SPIRAM-preferred
//     via bb_mem_arena's own allocator) on the FIRST SSE client connect. A
//     failed allocation fails soft — the connection is rejected (same 500
//     path the pre-existing xTaskCreateStatic-failure branch already
//     returns) and retried on the next connect; no crash. B1-492:
//     bb_event_routes_start()'s idle-reclaim tick (below) destroys this pool
//     once it goes fully idle (0 active clients, 0 acquired bundles, 0
//     pending corpses), returning to 0 standing bytes; it is recreated
//     lazily on the next connect, same as the very first one.
//   y — the pool is created eagerly at
//     bb_event_routes_register_routes_init() time over a permanent
//     static-BSS arena (unchanged PR E behavior) and is never destroyed —
//     the idle-reclaim tick is not created at all in this mode (nothing to
//     reclaim).
// ---------------------------------------------------------------------------

#define SSE_TASK_STACK_WORDS (4096 / sizeof(StackType_t))

typedef struct {
    StackType_t       stack[SSE_TASK_STACK_WORDS];
    StaticTask_t      tcb;
    TaskHandle_t      handle;    // set by events_handler right after xTaskCreateStatic; NULL = never used (or reaped)
    SemaphoreHandle_t done_sem;  // completion signal, created once on first acquire (see sse_bundle_on_acquire), reused across reissues
} sse_task_bundle_t;

// xTaskCreateStatic requires the stack buffer and TCB to be at least
// naturally aligned for their respective types; bb_mem_arena_alloc() returns
// _Alignof(max_align_t)-aligned storage, which is >= both — verify that
// assumption holds on this target rather than relying on it silently.
_Static_assert(_Alignof(max_align_t) >= _Alignof(StackType_t) &&
               _Alignof(max_align_t) >= _Alignof(StaticTask_t),
               "bb_mem_arena alignment insufficient for FreeRTOS static task storage");

static bb_pool_t s_sse_task_pool;  // NULL until sse_task_bundles_ensure() creates it

// Static arena sizing for the POOL_STATIC=y path — mirrors bb_pool's own
// SLOTS-mode math (bb_pool_arena_size_needed() in
// platform/host/bb_pool/bb_pool.c) rather than a single flat byte count.
//
// Only bb_mem_arena's internal header struct (private to bb_mem_arena.c) and
// bb_pool's own control struct (private to bb_pool.c) are truly
// capacity-independent — SSE_TASK_ARENA_HDR_ALLOWANCE_BYTES covers those
// two only, generously rounded. Everything else scales linearly with
// CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS (Kconfig range 1..32): the per-slot
// bundle storage, the free-list (one void* per slot), and TWO bitmaps
// (acquired + pending, the pending bitmap added by B1-479) each
// ceil(capacity/8) bytes. The prior flat 256-byte allowance covered all of
// this with a single capacity-independent constant, so it silently
// overflowed above whatever capacity happened to fit in 256 bytes —
// exactly at the boundary at the Kconfig default of 2. This formula tracks
// bb_pool's real per-slot cost instead, and sse_task_bundles_ensure() below
// cross-checks it at runtime against bb_pool_arena_size_needed() itself.
#define SSE_TASK_ARENA_HDR_ALLOWANCE_BYTES 192u

#define SSE_TASK_ALIGN_UP(n) \
    (((size_t)(n) + (_Alignof(max_align_t) - 1u)) & ~(size_t)(_Alignof(max_align_t) - 1u))

#define SSE_TASK_SLOT_STRIDE SSE_TASK_ALIGN_UP(sizeof(sse_task_bundle_t))
#define SSE_TASK_BUNDLES_BYTES \
    ((size_t)CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS * SSE_TASK_SLOT_STRIDE)
#define SSE_TASK_FREE_LIST_BYTES \
    SSE_TASK_ALIGN_UP((size_t)CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS * sizeof(void *))
#define SSE_TASK_BITMAP_BYTES \
    SSE_TASK_ALIGN_UP(((size_t)CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS + 7u) / 8u)

// Two bitmaps (acquired + pending) — see the comment block above.
#define SSE_TASK_ARENA_TOTAL_BYTES \
    (SSE_TASK_ARENA_HDR_ALLOWANCE_BYTES + SSE_TASK_BUNDLES_BYTES + \
     SSE_TASK_FREE_LIST_BYTES + 2u * SSE_TASK_BITMAP_BYTES)

_Static_assert(CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS == 0 ||
               (SSE_TASK_BUNDLES_BYTES / CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS) == SSE_TASK_SLOT_STRIDE,
               "SSE task bundle byte count overflowed size_t");

// Static-BSS backing buffer — only used for the POOL_STATIC=y path. Sized
// from the capacity-scaling formula above, not a flat constant — this is
// the compile-time enforcement of the "unreachable on failure" claim in
// sse_task_bundles_ensure(): the buffer is provably large enough for
// whatever CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS the Kconfig range (1..32)
// permits, rather than silently overflowing above a hardcoded threshold.
#if CONFIG_BB_EVENT_ROUTES_POOL_STATIC
static uint8_t s_sse_task_arena_buf[SSE_TASK_ARENA_TOTAL_BYTES]
    __attribute__((aligned(_Alignof(max_align_t))));
static bb_mem_arena_t s_sse_task_arena;
#endif

// Dedicated leaf mutex guarding the ENTIRE s_sse_task_pool lifetime
// (portable POSIX pthread — ESP-IDF ships a pthread layer; mirrors
// bb_mem_arena_tls's s_arena_mtx pattern and the sibling s_sse_pool_mtx in
// bb_event_routes_common.c). Separate mutex from s_sse_pool_mtx: distinct
// global state (s_sse_task_pool vs s_sse_bundles), distinct translation
// units, no ordering interaction between them.
//
// B1-484: bb_pool is not internally thread-safe (see bb_pool.h). Every
// bb_pool_acquire()/bb_pool_release() call against s_sse_task_pool MUST run
// under this mutex, not just the lazy sse_task_bundles_ensure() first-create
// section — bb_pool_release() (events_cleanup_fn, on the exiting SSE task)
// and bb_pool_acquire() (events_handler, on the httpd task) race across
// tasks with no other synchronization between them. Use the sse_pool_*()
// wrappers below rather than calling bb_pool_acquire/bb_pool_release on
// s_sse_task_pool directly, so every call site is covered.
//
// Bounded critical section: bb_pool_acquire/release never block (SLOTS mode
// is a bump/bitmap data structure, not a queue) — the callbacks run inside
// them (sse_bundle_reusable/sse_bundle_reap/sse_bundle_on_acquire) do only
// eTaskGetState()/vTaskDelete()/xSemaphoreCreateBinary(), none of which
// block or take another lock. Do not add a blocking call to any of those
// callbacks or to the sections below without re-auditing this bound.
//
// Lock ordering: sse_task_bundles_ensure() is called only from
// events_handler() (before any bb_event_routes lock is taken — the client
// has already been fully acquired via bb_event_routes_client_acquire_ex(),
// which releases s_topics_lock before returning) and from
// bb_event_routes_register_routes_init() before the HTTP server accepts
// requests. events_cleanup_fn calls sse_pool_release() after it has already
// deregistered the task and released the client slot (no bb_event_routes
// lock held at that point either). B1-492's sse_pool_reclaim_work_fn (below)
// also takes this mutex directly (not via the sse_pool_* wrappers, since it
// needs bb_pool_slots_reap_ready/pending_count/acquired_count rather than
// acquire/release) — it runs on the shared bb_timer_disp task, holding no
// other lock beforehand. This mutex is therefore always the innermost (and
// only) lock held during any of its critical sections — a non-recursive
// pthread_mutex is safe. Keep it that way: never call
// sse_pool_acquire()/sse_pool_release()/sse_task_bundles_ensure()/
// sse_pool_reclaim_work_fn's body from inside another critical section
// already holding this same mutex (it is not recursive — re-entering it
// self-deadlocks), and do not call anything from inside a locked section
// that could re-enter this mutex or take another bb_event_routes lock.
static pthread_mutex_t s_sse_task_pool_mtx = PTHREAD_MUTEX_INITIALIZER;

// Locked wrappers — the only call sites permitted to touch s_sse_task_pool
// via bb_pool_acquire()/bb_pool_release() (see the mutex comment above).
static sse_task_bundle_t *sse_pool_acquire(void)
{
    pthread_mutex_lock(&s_sse_task_pool_mtx);
    sse_task_bundle_t *b = (sse_task_bundle_t *)bb_pool_acquire(s_sse_task_pool);
    pthread_mutex_unlock(&s_sse_task_pool_mtx);
    return b;
}

static void sse_pool_release(sse_task_bundle_t *b)
{
    pthread_mutex_lock(&s_sse_task_pool_mtx);
    bb_pool_release(s_sse_task_pool, b);
    pthread_mutex_unlock(&s_sse_task_pool_mtx);
}

// on_acquire: lazily create the bundle's completion semaphore on its
// first-ever acquire. bb_pool zero-initializes SLOTS storage at pool
// creation (see bb_pool.c), so `done_sem == NULL` reliably distinguishes
// "never created" from "already created" across repeated reissues of the
// same bundle — a semaphore, once created, is reused for the bundle's
// lifetime rather than recreated per connection.
static void sse_bundle_on_acquire(void *ctx, void *slot)
{
    (void)ctx;
    sse_task_bundle_t *b = (sse_task_bundle_t *)slot;
    if (!b->done_sem) {
        b->done_sem = xSemaphoreCreateBinary();
        if (!b->done_sem) {
            bb_log_w(TAG, "SSE task bundle: xSemaphoreCreateBinary failed (heap pressure)");
        }
    }
    // Belt-and-suspenders: sse_bundle_reap() already clears handle on reap,
    // and a fresh slot is zero-inited by bb_pool — this assignment is
    // redundant but cheap, and keeps this function correct even if a future
    // caller path acquires a slot without going through reap first.
    b->handle = NULL;  // set by events_handler immediately after xTaskCreateStatic
}

// slot_reusable: single non-blocking check, no loop, no timeout — see the
// file-header comment above. NULL handle means the bundle has never been
// assigned a task (fresh from the free-list; bb_pool_acquire() only
// consults this callback once the free-list is empty, but a defensive NULL
// check keeps this function safe to call unconditionally). The actual
// state->action decision is delegated to the pure, host-tested
// sse_bundle_decide() (sse_bundle_decision.h) — only the FreeRTOS
// eTaskGetState() call itself stays here.
static bool sse_bundle_reusable(void *ctx, void *slot)
{
    (void)ctx;
    sse_task_bundle_t *b = (sse_task_bundle_t *)slot;
    sse_bundle_task_state_t state = !b->handle
        ? SSE_BUNDLE_TASK_NONE
        : (eTaskGetState(b->handle) == eSuspended ? SSE_BUNDLE_TASK_SUSPENDED
                                                   : SSE_BUNDLE_TASK_RUNNING);
    return sse_bundle_decide(state) != SSE_BUNDLE_ACTION_NOT_YET;
}

// slot_reap: external vTaskDelete of a confirmed-eSuspended corpse,
// invoked exactly once immediately before reissue. The completion
// semaphore is taken non-blocking purely as a diagnostic cross-check (the
// exiting task's tail always gives it strictly before calling
// vTaskSuspend(NULL), so this should never actually block or miss) — it is
// NOT part of the reuse gate itself, which is slot_reusable's
// eTaskGetState() check above. Gated on b->handle != NULL: a bundle whose
// task was never actually created (slot_reusable's NULL-handle fast path
// above) has no completion signal to observe — warning here would be a
// spurious log, not a real diagnostic.
static void sse_bundle_reap(void *ctx, void *slot)
{
    (void)ctx;
    sse_task_bundle_t *b = (sse_task_bundle_t *)slot;
    if (b->handle && b->done_sem && xSemaphoreTake(b->done_sem, 0) != pdTRUE) {
        bb_log_w(TAG, "SSE task bundle reaped as eSuspended but its completion signal was never observed");
    }
    if (b->handle) {
        vTaskDelete(b->handle);
    }
    b->handle = NULL;
}

// on_destroy: invoked by bb_pool_destroy() over EVERY slot (free, acquired,
// or pending) right before the backing arena — and therefore this
// sse_task_bundle_t storage itself — is freed. Releases the done_sem lazily
// created by sse_bundle_on_acquire() (B1-479); nothing ever deleted it
// before, so every idle-reclaim destroy cycle leaked one binary semaphore
// per pool slot. Safe to call unconditionally on a never-acquired slot
// (done_sem == NULL, since bb_pool zero-inits SLOTS storage at create time).
// No double-delete risk: the whole pool (and this slot's storage) is
// destroyed immediately after this call and recreated fresh — with a fresh
// zero-inited done_sem — on the next connect; the freed handle is never
// reachable again.
static void sse_bundle_on_destroy(void *ctx, void *slot)
{
    (void)ctx;
    sse_task_bundle_t *b = (sse_task_bundle_t *)slot;
    if (b->done_sem) {
        vSemaphoreDelete(b->done_sem);
        b->done_sem = NULL;
    }
    b->handle = NULL;
}

// Create the SSE-private task-bundle pool on demand. Idempotent — returns
// BB_OK immediately once s_sse_task_pool is set.
//
// STATIC path: called eagerly, once, from
// bb_event_routes_register_routes_init. The backing buffer is sized by the
// SSE_TASK_ARENA_TOTAL_BYTES formula above, which mirrors bb_pool's own
// SLOTS-mode sizing math and scales with CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS
// — so failure here should be unreachable across the full Kconfig capacity
// range (1..32). That claim is cross-checked below against
// bb_pool_arena_size_needed(), the actual runtime authority for the
// capacity-scaling portion of the math, before bb_mem_arena_init() ever runs —
// catching any future drift (e.g. bb_pool.c's private struct growing, or a
// third bitmap being added) instead of silently overflowing the arena the
// way the old flat 256-byte allowance did.
//
// Lazy-heap path (default): called from events_handler on the first SSE
// connect. See the file-header comment above for the fail-soft and
// no-idle-reclaim rationale.
static bb_err_t sse_task_bundles_ensure(void)
{
    pthread_mutex_lock(&s_sse_task_pool_mtx);

    if (s_sse_task_pool) {
        pthread_mutex_unlock(&s_sse_task_pool_mtx);
        return BB_OK;
    }

    bb_pool_cfg_t cfg = {
        .mode           = BB_POOL_MODE_SLOTS,
        .capacity       = CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS,
        .max_slot_bytes = sizeof(sse_task_bundle_t),
        .name           = "sse_task",
        .on_acquire     = sse_bundle_on_acquire,
        .slot_reusable  = sse_bundle_reusable,
        .slot_reap      = sse_bundle_reap,
        .on_destroy     = sse_bundle_on_destroy,
    };

#if CONFIG_BB_EVENT_ROUTES_POOL_STATIC
    // Runtime cross-check against bb_pool's own sizing authority — see the
    // function-header comment above. Not "provably unreachable" purely by
    // construction (bb_pool's private struct size isn't visible here at
    // compile time), but validated against the real formula every time this
    // runs, rather than only hoping the hand-mirrored macro above stays in
    // sync with bb_pool.c forever.
    size_t pool_need = bb_pool_arena_size_needed(&cfg);
    assert(pool_need > 0 && pool_need <= sizeof(s_sse_task_arena_buf));  // LCOV_EXCL_BR_LINE — buffer is sized to fit the full Kconfig capacity range
    bb_err_t err = bb_mem_arena_init(&s_sse_task_arena, s_sse_task_arena_buf,
                                     sizeof(s_sse_task_arena_buf));
    assert(err == BB_OK);  // LCOV_EXCL_BR_LINE — buffer is compile-time sized to fit
    err = bb_pool_create(&cfg, s_sse_task_arena, &s_sse_task_pool);
    assert(err == BB_OK);  // LCOV_EXCL_BR_LINE — see above
    (void)err;  // avoid unused-variable warning when NDEBUG compiles the assert out
    (void)pool_need;
    pthread_mutex_unlock(&s_sse_task_pool_mtx);
    return BB_OK;
#else
    bb_err_t err = bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, &s_sse_task_pool);
    if (err != BB_OK) {
        bb_log_w(TAG, "SSE task pool: heap alloc failed (%d); connection rejected, retrying next connect", err);
        pthread_mutex_unlock(&s_sse_task_pool_mtx);
        return BB_ERR_NO_SPACE;
    }
    pthread_mutex_unlock(&s_sse_task_pool_mtx);
    return BB_OK;
#endif
}

// ---------------------------------------------------------------------------
// Idle-reclaim tick (B1-492) — lazy-heap path only. Compiled out entirely
// under CONFIG_BB_EVENT_ROUTES_POOL_STATIC=y: that pool is a permanent
// eager-BSS arena created once at boot and never destroyed — there is
// nothing to reclaim, so no timer is created (see bb_event_routes_start()).
//
// Timer-callback-signal-only convention: bb_pool_destroy() frees memory
// (bb_mem_arena_destroy -> bb_mem_free), which must never run on the esp_timer
// service task. bb_event_routes_start() therefore arms this work_fn via
// bb_timer_deferred_periodic_create(), NOT bb_timer_periodic_create() — the
// shared bb_timer_disp task runs work_fn in real task context (its own
// stack, preemptible, free to lock/allocate/free), off the esp_timer
// service task entirely.
// ---------------------------------------------------------------------------
#if !CONFIG_BB_EVENT_ROUTES_POOL_STATIC

// Kconfig-bridged interval — this file is ESP-IDF-only (platform/espidf/),
// so CONFIG_BB_EVENT_ROUTES_IDLE_RECLAIM_MS is always available via
// sdkconfig.h (already transitively included via bb_core/bb_http); no
// host-fallback bridge needed here (contrast bb_clock.h, which is shared
// with host builds and does need one).
#define SSE_POOL_RECLAIM_INTERVAL_MS ((uint32_t)CONFIG_BB_EVENT_ROUTES_IDLE_RECLAIM_MS)

static bb_periodic_timer_t s_sse_reclaim_timer = NULL;

// Runs on the shared bb_timer_disp task (see the file-header comment above),
// never on the esp_timer service task. Locked for its entire body — the
// critical section is bounded: bb_pool_slots_reap_ready() only performs
// eTaskGetState()/vTaskDelete() per pending slot (same bound already
// documented for s_sse_task_pool_mtx above), and bb_pool_destroy() on the
// lazy-heap path is a single bb_mem_free() call, not a blocking IDF call.
static void sse_pool_reclaim_work_fn(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s_sse_task_pool_mtx);

    if (!s_sse_task_pool) {
        // Never connected yet, or already reclaimed by a prior tick —
        // nothing to do until the next SSE connect lazily recreates it.
        pthread_mutex_unlock(&s_sse_task_pool_mtx);
        return;
    }

    // Drain every corpse that has already reached eSuspended, independent
    // of whether any connect attempt is currently pending on this pool —
    // this is what lets a corpse get reaped even if nobody reconnects.
    bb_pool_slots_reap_ready(s_sse_task_pool);

    // acquired_count closes a race active_clients + pending_count alone
    // cannot see: events_cleanup_fn calls bb_event_routes_client_release()
    // (decrementing active_clients) BEFORE sse_pool_release() (which is what
    // actually moves the bundle out of the "acquired" bitmap and into
    // "pending"). Between those two calls the exiting task is still
    // executing on this exact bundle's stack — acquired_count catches that
    // in-flight window even though it is neither an active client nor yet a
    // pending corpse.
    size_t pending  = bb_pool_slots_pending_count(s_sse_task_pool);
    size_t acquired = bb_pool_slots_acquired_count(s_sse_task_pool);
    size_t active   = bb_event_routes_active_client_count();

    if (sse_pool_reclaim_decide(active, acquired, pending) == SSE_POOL_RECLAIM_DESTROY) {
        // Take the handle and null the global before dropping the lock so a
        // concurrent events_handler on the httpd task either sees NULL
        // (recreates lazily via sse_task_bundles_ensure()) or never observes
        // a half-destroyed pool — sse_pool_acquire()/sse_pool_release() and
        // sse_task_bundles_ensure() all take this same mutex.
        bb_pool_t victim = s_sse_task_pool;
        s_sse_task_pool = NULL;
        pthread_mutex_unlock(&s_sse_task_pool_mtx);
        bb_pool_destroy(victim);
        bb_log_i(TAG, "idle-reclaim: SSE task pool destroyed (0 active clients, 0 acquired, 0 pending corpses)");
        return;
    }

    pthread_mutex_unlock(&s_sse_task_pool_mtx);
}

#endif // !CONFIG_BB_EVENT_ROUTES_POOL_STATIC

// B1-492: self-registers at PRE_HTTP tier (mirrors bb_pub_start()) so
// bb_event_routes_register_routes_init (REGULAR tier, below) stays pure
// httpd route-attach with no timer/task creation. State-init + timer-create
// + timer-arm only — see the file-header comment above for why the timer
// itself must defer its work off the esp_timer service task.
bb_err_t bb_event_routes_start(void)
{
#if CONFIG_BB_EVENT_ROUTES_POOL_STATIC
    // Eager-BSS pool: nothing to reclaim, ever. Deliberate no-op — do not
    // create a timer that would just spin doing nothing every tick.
    return BB_OK;
#else
    // Idempotent: unlike bb_pub_start()'s fully-static, single-caller
    // pattern, this is a public function an external caller could invoke
    // more than once. A second call while the reclaim timer is already
    // armed must not create/arm a duplicate timer.
    if (s_sse_reclaim_timer != NULL) {
        return BB_OK;
    }
    bb_err_t err = bb_timer_deferred_periodic_create(sse_pool_reclaim_work_fn, NULL,
                                                     "sse_reclaim", &s_sse_reclaim_timer);
    if (err != BB_OK) {
        bb_log_e(TAG, "idle-reclaim: failed to create timer: %d", err);
        return err;
    }
    err = bb_timer_periodic_start(s_sse_reclaim_timer,
                                  (uint64_t)SSE_POOL_RECLAIM_INTERVAL_MS * 1000ULL);
    if (err != BB_OK) {
        bb_log_e(TAG, "idle-reclaim: failed to start timer: %d", err);
        return err;
    }
    bb_log_i(TAG, "idle-reclaim tick armed (%" PRIu32 " ms)", SSE_POOL_RECLAIM_INTERVAL_MS);
    return BB_OK;
#endif
}

// ---------------------------------------------------------------------------
// Port: SemaphoreHandle_t recursive mutex per slot. notify() is unused on
// ESP-IDF because the SSE task polls with a short timeout.
// ---------------------------------------------------------------------------

void *bb_event_routes_port_lock_create(void)
{
    return (void *)xSemaphoreCreateRecursiveMutex();
}

void bb_event_routes_port_lock_destroy(void *lock)
{
    if (lock) vSemaphoreDelete((SemaphoreHandle_t)lock);
}

void bb_event_routes_port_lock(void *lock)
{
    if (lock) xSemaphoreTakeRecursive((SemaphoreHandle_t)lock, portMAX_DELAY);
}

void bb_event_routes_port_unlock(void *lock)
{
    if (lock) xSemaphoreGiveRecursive((SemaphoreHandle_t)lock);
}

void bb_event_routes_port_notify(void *lock) { (void)lock; }

// Binary semaphore: capture_cb signals after enqueue; sse_task waits with a
// heartbeat-sized timeout so it wakes immediately on new events and emits a
// keepalive ping otherwise — no polling, no extra latency.
void *bb_event_routes_port_event_create(void)
{
    return (void *)xSemaphoreCreateBinary();
}

void bb_event_routes_port_event_destroy(void *event)
{
    if (event) vSemaphoreDelete((SemaphoreHandle_t)event);
}

void bb_event_routes_port_event_signal(void *event)
{
    if (event) xSemaphoreGive((SemaphoreHandle_t)event);
}

bool bb_event_routes_port_event_wait(void *event, uint32_t timeout_ms)
{
    if (!event) return false;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake((SemaphoreHandle_t)event, ticks) == pdTRUE;
}

// ---------------------------------------------------------------------------
// Per-client SSE task
// ---------------------------------------------------------------------------

typedef struct {
    bb_http_request_t *req;
    bb_event_routes_client_t *client;
    sse_task_bundle_t *bundle;  // this task's own leased bundle — needed by the exit tail (sse_task_done)
    void *event;
    bool has_more;  // drain one frame; more may be queued
} sse_task_arg_t;

// wait_fn: event-driven drain. Waits on the per-client signal up to timeout_ms,
// then drains one frame. has_more=true skips the wait and drains directly so
// all queued frames are flushed without counting spurious idle time.
static int events_wait_fn(void *ctx, char *buf, size_t buflen, uint32_t timeout_ms)
{
    sse_task_arg_t *t = (sse_task_arg_t *)ctx;

    if (t->has_more) {
        size_t n = bb_event_routes_drain_frame(t->client, buf, buflen);
        if (n > 0) return (int)n;
        t->has_more = false;
        // Queue exhausted — fall through to a real wait.
    }

    bool signaled = bb_event_routes_port_event_wait(t->event, timeout_ms);
    if (!signaled) return 0;  // idle timeout

    size_t n = bb_event_routes_drain_frame(t->client, buf, buflen);
    if (n == 0) return 0;  // signaled but nothing queued
    t->has_more = true;    // assume more frames; drain next call
    return (int)n;
}

// cleanup_fn: deregister this task, release the client slot, release this
// task's bundle lease back to the pool, and free the arg struct. Runs on
// the SSE task itself, before bb_sse_writer_run's done_fn tail
// (sse_task_done, below) actually suspends the task.
//
// Deregister MUST happen before client_release: release() sets the client
// slot's in_use=false, which makes that slot immediately reusable by a
// concurrent acquire on the other core.
//
// sse_pool_release(t->bundle) marks this bundle
// pending-reap (its slot_reusable/slot_reap were configured in
// sse_task_bundles_ensure()) rather than immediately reissuable — a future
// bb_pool_acquire() will not hand this bundle out until slot_reusable
// confirms eTaskGetState(handle)==eSuspended, which cannot happen until
// AFTER this very task calls vTaskSuspend(NULL) in sse_task_done() below.
// It is therefore safe to release the bundle here, before this task has
// actually suspended: the bundle cannot be reissued in the meantime.
static void events_cleanup_fn(void *ctx)
{
    sse_task_arg_t *t = (sse_task_arg_t *)ctx;
    bb_err_t drc = bb_task_deregister(xTaskGetCurrentTaskHandle());
    if (drc != BB_OK) {
        // Benign/expected — e.g. BB_ERR_NOT_FOUND when the earlier register
        // call failed. Debug-level only; not actionable at runtime.
        bb_log_d(TAG, "sse task deregister: %d", drc);
    }
    bb_event_routes_client_release(t->client);
    if (t->bundle) {
        sse_pool_release(t->bundle);
    }
    // NOTE: `t` itself is freed by sse_task_done(), not here — done_fn runs
    // after this function returns and still needs t->bundle/t->done_sem.
}

// done_fn (replaces the default vTaskDelete(NULL) self-delete): as this
// task's absolute last act, give the bundle's completion semaphore, free
// the arg struct, then vTaskSuspend(NULL) — never self-delete. The task
// becomes a suspended "corpse" occupying its bundle's static stack/TCB;
// sse_bundle_reusable polls eTaskGetState() to detect this, and
// sse_bundle_reap() performs the external vTaskDelete() once a future
// connection wants to reuse the bundle. See the SSE task-bundle pool
// file-header comment for the full B1-484/B1-492 rationale. Never returns.
static void sse_task_done(void *ctx)
{
    sse_task_arg_t *t = (sse_task_arg_t *)ctx;
    if (t->bundle && t->bundle->done_sem) {
        xSemaphoreGive(t->bundle->done_sem);
    }
    bb_mem_free(t);
    vTaskSuspend(NULL);
}

static void sse_task(void *arg)
{
    sse_task_arg_t *t = (sse_task_arg_t *)arg;
    bb_http_request_t *req = t->req;
    t->event = bb_event_routes_client_event(t->client);
    t->has_more = false;
    const uint32_t hb_ms = bb_event_routes_heartbeat_ms();
    bb_sse_writer_run(req, ": connected\nretry: 5000\n\n",
                      events_wait_fn, events_cleanup_fn, sse_task_done, t,
                      hb_ms, hb_ms);
    // sse_task_done() calls vTaskSuspend(NULL) — never returns.
}

// ---------------------------------------------------------------------------
// HTTP handler
// ---------------------------------------------------------------------------

static bb_err_t events_handler(bb_http_request_t *req)
{
    // Parse optional ?topic= query parameter
    char topic_buf[32] = {0};
    const char *topic_filter = NULL;
    if (bb_http_req_query_key_value(req, "topic", topic_buf, sizeof(topic_buf)) == BB_OK) {
        topic_filter = topic_buf;
    }

    bb_event_routes_client_t *client = NULL;
    bb_err_t err = bb_event_routes_client_acquire_ex(&client, topic_filter);
    if (err != BB_OK) {
        // B1-561: unified through sse_connect_error_status() — same
        // BB_ERR_NO_SPACE-vs-other mapping as the task-pool-ensure branch
        // below, no behavior change from the prior explicit NO_SPACE/other
        // split.
        int status = sse_connect_error_status(err);
        bb_http_resp_set_status(req, status);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        if (status == 503) {
            bb_http_resp_json_obj_set_str(&obj, "error", "max_clients");
        } else {
            bb_http_resp_json_obj_set_str(&obj, "error", "event routes not initialized");
        }
        bb_http_resp_json_obj_end(&obj);
        return (status == 503) ? BB_OK : err;
    }

    // Lazy-heap default: create the task-bundle pool on the first-ever SSE
    // connect. No-op once created (either backing) — see
    // sse_task_bundles_ensure(). A failed heap allocation fails soft: the
    // connection is rejected and retried on the next connect. B1-561: a
    // transient BB_ERR_NO_SPACE (heap pressure) maps to the same retryable
    // 503 idiom as the sibling payload-pool and max_clients acquire
    // failures below (EventSource auto-retries); any other error is a
    // genuine failure and stays a hard 500. sse_connect_error_status() is
    // the pure, host-testable status decision — see
    // sse_connect_error_decision.h.
    bb_err_t pool_err = sse_task_bundles_ensure();
    if (pool_err != BB_OK) {
        bb_event_routes_client_release(client);
        int status = sse_connect_error_status(pool_err);
        bb_http_resp_set_status(req, status);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        if (status == 503) {
            // B1-561: distinct body value ("busy") from the genuine
            // max_clients path — this 503 is transient heap pressure at the
            // lazy first-connect pool allocation, not exhaustion of the
            // client-slot pool. Separately counted (below) so the two
            // causes stay observable even though both retry the same way.
            bb_event_routes_note_pool_ensure_deferred();
            bb_http_resp_json_obj_set_str(&obj, "error", "busy");
        } else {
            bb_http_resp_json_obj_set_str(&obj, "error", "event routes not initialized");
        }
        bb_http_resp_json_obj_end(&obj);
        return (status == 503) ? BB_OK : pool_err;
    }

    // B1-484/B1-492 reap gate: this bb_pool_acquire() call is the
    // non-blocking, single-shot reuse check on the one esp_http_server
    // task. If a slot's prior occupant task hasn't yet been confirmed
    // eSuspended (sse_bundle_reusable), bb_pool_acquire() returns NULL here
    // immediately — no wait, no loop, no deadline — and this handler
    // fast-rejects with the same 503 idiom used for max_clients exhaustion
    // (EventSource clients auto-retry). This can never permanently strand a
    // slot: the corpse eventually reaches eSuspended and a later connect
    // succeeds.
    sse_task_bundle_t *bundle = sse_pool_acquire();
    if (!bundle) {
        // B1-492: this specific 503 (reap-gate deferral, not max_clients
        // exhaustion) is separately counted so boot-contention reuse
        // pressure stays observable now that it manifests as transient
        // 503-retries instead of B1-484's permanent strand.
        bb_event_routes_note_slot_reuse_deferred();
        bb_event_routes_client_release(client);
        bb_http_resp_set_status(req, 503);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "max_clients");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bb_http_request_t *async_req = NULL;
    if (bb_http_req_async_handler_begin(req, &async_req) != BB_OK) {
        sse_pool_release(bundle);
        bb_event_routes_client_release(client);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "Async init failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }

    sse_task_arg_t *arg = (sse_task_arg_t *)bb_malloc_prefer_spiram(sizeof(*arg));
    if (!arg) {
        sse_pool_release(bundle);
        bb_event_routes_client_release(client);
        bb_http_req_async_handler_complete(async_req);
        return BB_ERR_NO_SPACE;
    }
    arg->req = async_req;
    arg->client = client;
    arg->bundle = bundle;

    // Each concurrent SSE task gets a unique registry name — a literal
    // "sse_events" for every task would collide in the registry (dup-name
    // rejection hides all but the first). events_handler runs exclusively
    // on the single esp_http_server task (no worker pool — see breadboard
    // CLAUDE.md), so this monotonic counter needs no atomic/lock. Bundle
    // leases are no longer index-addressed to a client slot (B1-492), so
    // naming is decoupled from client slot index entirely.
    static uint32_t s_sse_task_seq;
    char task_name[BB_TASK_NAME_MAX];
    snprintf(task_name, sizeof(task_name), "sse_%" PRIu32, s_sse_task_seq++);

    TaskHandle_t th = NULL;
    bb_task_config_t sse_cfg = {
        .entry       = sse_task,
        .name        = task_name,
        .arg         = arg,
        .stack_bytes = SSE_TASK_STACK_WORDS * sizeof(StackType_t),
        .priority    = 1,
        .core        = BB_TASK_CORE_ANY,
        .backing     = BB_TASK_BACKING_STATIC,
        .stack_buf   = bundle->stack,
        .tcb_buf     = &bundle->tcb,
        .wdt_arm     = false,
    };
    if (bb_task_create(&sse_cfg, (void **)&th) != BB_OK) {
        bb_mem_free(arg);
        sse_pool_release(bundle);
        bb_event_routes_client_release(client);
        bb_http_req_async_handler_complete(async_req);
        return BB_ERR_INVALID_STATE;
    }
    bundle->handle = th;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static const bb_route_response_t s_events_responses[] = {
    { 200, "text/event-stream", NULL,
      "Server-Sent Events stream of bb_event topic posts. Each event has "
      "`event:` (topic name), `data:` (JSON payload posted by the producer), "
      "and `id:` (monotonic per-stream). Topic must have been attached via "
      "bb_event_routes_attach." },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "event routes not initialized, or async handler init failed" },
    { 503, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\",\"enum\":[\"max_clients\",\"busy\"]}},"
      "\"required\":[\"error\"]}",
      "maximum concurrent clients reached (\"max_clients\"), or transient "
      "heap pressure at connect (\"busy\", B1-561; see pool_ensure_deferred "
      "on GET /api/diag/events) — both retryable" },
    { 0 },
};

static const bb_route_param_t s_events_params[] = {
    {
        .name        = "topic",
        .in          = "query",
        .description = "Filter the SSE stream to a single topic name. Available topics are "
                       "listed by GET /api/diag/events. Omit to receive all attached topics.",
        .required    = false,
        .schema_type = "string",
    },
};

static const bb_route_t s_events_route = {
    .method            = BB_HTTP_GET,
    .path              = "/api/events",
    .tag               = "events",
    .summary           = "Stream bb_event topic posts via SSE",
    .responses         = s_events_responses,
    .handler           = events_handler,
    .parameters        = s_events_params,
    .parameters_count  = 1,
};

// ---------------------------------------------------------------------------
// Diag handler: GET /api/diag/events
// ---------------------------------------------------------------------------

static bb_err_t diag_events_handler(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    // Capture now_us once for age computation across all topics.
    int64_t now_us = (int64_t)bb_timer_now_us();

    // "topics" array
    bb_http_resp_json_obj_set_arr_begin(&obj, "topics");
    size_t n = bb_event_routes_topic_count();
    for (size_t i = 0; i < n; i++) {
        const char *name = NULL;
        bb_event_ring_t ring = NULL;
        if (bb_event_routes_topic_info(i, &name, &ring) != BB_OK) continue;

        bb_http_resp_json_obj_set_obj_begin(&obj, NULL);
        bb_http_resp_json_obj_set_str(&obj, "name", name ? name : "");

        if (ring) {
            bb_http_resp_json_obj_set_int(&obj, "ring_capacity", (int64_t)bb_event_ring_capacity(ring));
            bb_http_resp_json_obj_set_int(&obj, "ring_count",    (int64_t)bb_event_ring_count(ring));

            uint32_t last_id = 0;
            size_t   last_sz = 0;
            int64_t  last_us = 0;
            if (bb_event_ring_last_entry_info(ring, &last_id, &last_sz, &last_us) == BB_OK) {
                int64_t age_ms = (last_us > 0 && now_us >= last_us)
                                 ? (now_us - last_us) / 1000
                                 : 0;
                bb_http_resp_json_obj_set_int(&obj, "last_id",          (int64_t)last_id);
                bb_http_resp_json_obj_set_int(&obj, "last_post_age_ms", age_ms);
                bb_http_resp_json_obj_set_int(&obj, "last_size",        (int64_t)last_sz);
            } else {
                bb_http_resp_json_obj_set_int(&obj, "last_id",          0);
                bb_http_resp_json_obj_set_int(&obj, "last_post_age_ms", 0);
                bb_http_resp_json_obj_set_int(&obj, "last_size",        0);
            }
        } else {
            bb_http_resp_json_obj_set_int(&obj, "ring_capacity",    0);
            bb_http_resp_json_obj_set_int(&obj, "ring_count",       0);
            bb_http_resp_json_obj_set_int(&obj, "last_id",          0);
            bb_http_resp_json_obj_set_int(&obj, "last_post_age_ms", 0);
            bb_http_resp_json_obj_set_int(&obj, "last_size",        0);
        }
        bb_http_resp_json_obj_set_obj_end(&obj);
    }
    bb_http_resp_json_obj_set_arr_end(&obj);

    bb_http_resp_json_obj_set_int(&obj, "max_clients",    (int64_t)CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS);
    bb_http_resp_json_obj_set_int(&obj, "active_clients", (int64_t)bb_event_routes_active_client_count());
    bb_http_resp_json_obj_set_int(&obj, "slot_reuse_deferred",
                                   (int64_t)bb_event_routes_slot_reuse_deferred_count());
    bb_http_resp_json_obj_set_int(&obj, "pool_ensure_deferred",
                                   (int64_t)bb_event_routes_pool_ensure_deferred_count());

    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_diag_events_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"topics\":{\"type\":\"array\",\"items\":{"
      "\"type\":\"object\","
      "\"properties\":{"
      "\"name\":{\"type\":\"string\"},"
      "\"ring_capacity\":{\"type\":\"integer\"},"
      "\"ring_count\":{\"type\":\"integer\"},"
      "\"last_id\":{\"type\":\"integer\"},"
      "\"last_post_age_ms\":{\"type\":\"integer\"},"
      "\"last_size\":{\"type\":\"integer\"}},"
      "\"required\":[\"name\",\"ring_capacity\",\"ring_count\","
      "\"last_id\",\"last_post_age_ms\",\"last_size\"]}},"
      "\"max_clients\":{\"type\":\"integer\"},"
      "\"active_clients\":{\"type\":\"integer\"},"
      "\"slot_reuse_deferred\":{\"type\":\"integer\"},"
      "\"pool_ensure_deferred\":{\"type\":\"integer\"}},"
      "\"required\":[\"topics\",\"max_clients\",\"active_clients\",\"slot_reuse_deferred\","
      "\"pool_ensure_deferred\"]}",
      "topic discovery and ring-buffer diagnostics for /api/events — "
      "ring_count=0 means no replay data; last_post_age_ms=0 means no events captured yet; "
      "slot_reuse_deferred (B1-492) counts transient reap-gate 503s (task-bundle reuse pending); "
      "pool_ensure_deferred (B1-561) counts transient heap-pressure 503s at the lazy first-connect "
      "pool allocation; both are distinct from max_clients-exhaustion 503s" },
    { 0 },
};

static const bb_route_t s_diag_events_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/events",
    .tag       = "diag",
    .summary   = "List attached SSE topics with ring-buffer diagnostics",
    .responses = s_diag_events_responses,
    .handler   = diag_events_handler,
};

// Forward declaration: implemented in bb_event_routes_spiram.c (same component).
// Sets SPIRAM-preferred allocator for per-client queue buffers before any
// client slot is allocated.
void bb_event_routes_spiram_init(void);

static bb_err_t bb_event_routes_register_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_event_routes_spiram_init();
#if CONFIG_BB_EVENT_ROUTES_POOL_STATIC
    {
        // Eager, deterministic pool from boot — unchanged PR E behavior. The
        // lazy-heap default backing is created later, on the first SSE
        // connect (see sse_task_bundles_ensure()).
        bb_err_t pool_err = sse_task_bundles_ensure();
        assert(pool_err == BB_OK);  // static backing is provably unreachable-on-failure, see sse_task_bundles_ensure()
        (void)pool_err;  // avoid unused-variable warning when NDEBUG compiles the assert out
    }
#endif
    bb_err_t err = bb_event_routes_init(NULL);
    if (err != BB_OK) return err;
    err = bb_http_register_described_route(server, &s_events_route);
    if (err != BB_OK) return err;
    err = bb_http_register_described_route(server, &s_diag_events_route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "registered /api/events + /api/diag/events");
    return BB_OK;
}

#if CONFIG_BB_EVENT_ROUTES_AUTOREGISTER
static bb_err_t bb_event_routes_reserve_routes(void)
{
    bb_http_reserve_routes(2);  // GET /api/events + GET /api/diag/events
    return BB_OK;
}
BB_INIT_REGISTER_PRE_HTTP(bb_event_routes, bb_event_routes_reserve_routes);
BB_INIT_REGISTER_PRE_HTTP(bb_event_routes_start, bb_event_routes_start);
BB_INIT_REGISTER(bb_event_routes, bb_event_routes_register_routes_init);
#endif
