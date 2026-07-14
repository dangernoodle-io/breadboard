// Must come before any system header — glibc <features.h> latches feature
// flags on first include, and we need PTHREAD_MUTEX_RECURSIVE +
// pthread_mutexattr_settype which glibc gates on _GNU_SOURCE
// (or _XOPEN_SOURCE >= 500). macOS / BSD libc expose them unconditionally,
// so this only surfaces on the Linux host build (CI).
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "bb_event.h"
#include "bb_event_port.h"
#include "bb_log.h"
#include "bb_core.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>

static const char *TAG = "bb_event_host";

// Bound on how long bb_event_port_reset_for_test() will wait for the
// dispatcher thread to drain a pending entry before giving up and tearing
// the port down anyway (see bb_event_port_reset_for_test below).
#define BB_EVENT_DRAIN_WAIT_TIMEOUT_MS 500

// ---------------------------------------------------------------------------
// Port state
// ---------------------------------------------------------------------------

struct bb_event_port {
    uint8_t *queue_memory;        // fixed-stride array
    size_t slot_size;             // sizeof(bb_event_queue_entry_t) + max_payload
    size_t queue_depth;
    size_t max_payload;

    size_t head;                  // write position
    size_t tail;                  // read position
    size_t count;                 // entries in queue

    pthread_mutex_t mutex;
    pthread_cond_t cond;           // producer -> dispatcher: "queue has entries"
    pthread_cond_t drained_cond;   // dispatcher -> teardown: "queue just hit 0"
    pthread_t dispatcher_thread;
    bool thread_running;
    bool sync_mode;               // BB_EVENT_HOST_SYNC=1: skip thread, use bb_event_port_drain synchronously
};

static bb_event_port_t *s_port = NULL;

#ifdef BB_EVENT_TESTING
static void *(*s_port_malloc)(size_t) = malloc;

// Test-only: delays the dispatcher thread's very first iteration (before it
// ever touches the mutex or the "has entries" condvar), so a test can
// deterministically simulate a freshly-spawned dispatcher thread that the OS
// hasn't scheduled yet -- the exact race bb_event_port_reset_for_test's
// drain-wait guards against. Reset to 0 by bb_event_port_reset_for_test so
// it never leaks into a later test.
static uint32_t s_test_dispatcher_startup_delay_ms = 0;

void bb_event_port_test_set_dispatcher_startup_delay_ms(uint32_t ms)
{
    s_test_dispatcher_startup_delay_ms = ms;
}

// Test-only: wall-clock duration (ms) of the most recent drain-wait segment
// inside bb_event_port_reset_for_test (the pthread_cond_timedwait loop only
// -- NOT the surrounding pthread_join, whose own duration is dominated by
// the dispatcher's startup delay regardless of whether the drain-wait ran,
// which would otherwise mask this measurement). -1 if reset_for_test hasn't
// run a drain-wait since the last call (nothing was queued, or the port
// wasn't running).
static long s_test_last_drain_wait_ms = -1;

long bb_event_port_test_get_last_drain_wait_ms(void)
{
    return s_test_last_drain_wait_ms;
}

// nanosleep() can return early on EINTR (this suite runs dense multithreaded
// tests elsewhere that spawn/join their own timer threads; a stray signal
// delivery is plausible), which would make the startup-delay hook above
// unreliable for tests that need it to genuinely elapse in full. Loop on the
// remaining time so the delay is honored even across interruptions.
static void test_nanosleep_full(const struct timespec *req)
{
    struct timespec remaining = *req;
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
        // remaining now holds the time left; retry.
    }
}
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Pure (no clock/global access) so it can be exercised directly by tests
// with crafted `now` values -- whether `now.tv_nsec + timeout_ms` overflows
// a whole second depends on the wall-clock fraction of a second at the
// instant bb_event_port_reset_for_test happens to call this, which is
// effectively a coin flip; calling clock_gettime() directly from that
// function would make the overflow-normalization branch below just as
// scheduling-flaky under host coverage as the bug this file exists to fix.
static struct timespec compute_drain_deadline(struct timespec now, uint32_t timeout_ms)
{
    struct timespec deadline = now;
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000 * 1000;
    if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000 * 1000 * 1000;
    }
    return deadline;
}

#ifdef BB_EVENT_TESTING
struct timespec bb_event_port_test_compute_drain_deadline(struct timespec now, uint32_t timeout_ms)
{
    return compute_drain_deadline(now, timeout_ms);
}
#endif

static uint8_t *get_slot(size_t idx)
{
    if (!s_port || idx >= s_port->queue_depth) return NULL;
    return s_port->queue_memory + (idx * s_port->slot_size);
}

static void *get_slot_payload(uint8_t *slot)
{
    return slot + sizeof(bb_event_queue_entry_t);
}

// ---------------------------------------------------------------------------
// Dispatcher thread
// ---------------------------------------------------------------------------

static void *dispatcher_loop(void *arg)
{
    (void)arg;

#ifdef BB_EVENT_TESTING
    if (s_test_dispatcher_startup_delay_ms > 0) {
        struct timespec startup_ts = {
            .tv_sec  = s_test_dispatcher_startup_delay_ms / 1000,
            .tv_nsec = (s_test_dispatcher_startup_delay_ms % 1000) * 1000 * 1000,
        };
        test_nanosleep_full(&startup_ts);
    }
#endif
    bb_log_d(TAG, "dispatcher thread started");

    while (s_port && s_port->thread_running) {
        pthread_mutex_lock(&s_port->mutex);

        // Wait for queue to have entries
        while (s_port->thread_running && s_port->count == 0) {
            pthread_cond_wait(&s_port->cond, &s_port->mutex);
        }

        if (!s_port->thread_running) {
            pthread_mutex_unlock(&s_port->mutex);
            break;
        }

        if (s_port->count == 0) {
            pthread_mutex_unlock(&s_port->mutex);
            continue;
        }

        // Pop one entry from queue
        uint8_t *slot = get_slot(s_port->tail);
        bb_event_queue_entry_t entry;
        memcpy(&entry, slot, sizeof(entry));
        void *payload_ptr = get_slot_payload(slot);
        uint8_t payload[s_port->max_payload];
        if (entry.size > 0) {
            memcpy(payload, payload_ptr, entry.size);
        }

        s_port->tail = (s_port->tail + 1) % s_port->queue_depth;
        s_port->count--;

        // Wake anyone (bb_event_port_reset_for_test) waiting for the queue
        // to fully drain, while still holding the mutex their predicate
        // check needs -- signalling after unlock would risk the drained
        // check racing this update.
        if (s_port->count == 0) {
            pthread_cond_broadcast(&s_port->drained_cond);
        }

        pthread_mutex_unlock(&s_port->mutex);

        // Dispatch outside the lock
        bb_event_common_dispatch(&entry, payload);
    }

    bb_log_d(TAG, "dispatcher thread stopped");
    return NULL;
}

// ---------------------------------------------------------------------------
// Port API
// ---------------------------------------------------------------------------

bb_err_t bb_event_port_init(size_t queue_depth, size_t max_payload,
                            size_t stack_size, int task_priority)
{
    (void)stack_size;
    (void)task_priority;

    if (s_port) return BB_OK;  // idempotent

    if (queue_depth == 0) queue_depth = 16;
    if (max_payload == 0) max_payload = 256;

#ifdef BB_EVENT_TESTING
    s_port = (bb_event_port_t *)s_port_malloc(sizeof(bb_event_port_t));
#else
    s_port = (bb_event_port_t *)malloc(sizeof(bb_event_port_t));
#endif
    if (!s_port) return BB_ERR_NO_SPACE;

    s_port->slot_size = sizeof(bb_event_queue_entry_t) + max_payload;
    s_port->queue_depth = queue_depth;
    s_port->max_payload = max_payload;
    s_port->head = 0;
    s_port->tail = 0;
    s_port->count = 0;
    s_port->thread_running = false;

    // Check for sync mode env var
    const char *sync_env = getenv("BB_EVENT_HOST_SYNC");
    s_port->sync_mode = (sync_env && sync_env[0] == '1');

    // Allocate queue memory
    s_port->queue_memory = (uint8_t *)malloc(s_port->slot_size * queue_depth);
    if (!s_port->queue_memory) {
        free(s_port);
        s_port = NULL;
        return BB_ERR_NO_SPACE;
    }

    // Initialize mutex (recursive)
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_port->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    // Initialize condition variables
    pthread_cond_init(&s_port->cond, NULL);
    pthread_cond_init(&s_port->drained_cond, NULL);

    // Spawn dispatcher thread (unless in sync mode)
    if (!s_port->sync_mode) {
        s_port->thread_running = true;
        int ret = pthread_create(&s_port->dispatcher_thread, NULL, dispatcher_loop, NULL);
        if (ret != 0) {
            pthread_cond_destroy(&s_port->drained_cond);
            pthread_cond_destroy(&s_port->cond);
            pthread_mutex_destroy(&s_port->mutex);
            free(s_port->queue_memory);
            free(s_port);
            s_port = NULL;
            bb_log_e(TAG, "pthread_create failed: %d", ret);
            return BB_ERR_INVALID_STATE;
        }
    }

    bb_log_i(TAG, "port_init: queue_depth=%zu max_payload=%zu sync=%d",
             queue_depth, max_payload, s_port->sync_mode);
    return BB_OK;
}

bb_err_t bb_event_port_enqueue(const bb_event_queue_entry_t *hdr,
                               const void *payload)
{
    if (!s_port || !hdr) return BB_ERR_INVALID_STATE;
    if (hdr->size > s_port->max_payload) return BB_ERR_INVALID_ARG;

    pthread_mutex_lock(&s_port->mutex);

    // Check if queue is full
    if (s_port->count >= s_port->queue_depth) {
        pthread_mutex_unlock(&s_port->mutex);
        bb_log_w(TAG, "queue full: %zu/%zu", s_port->count, s_port->queue_depth);
        return BB_ERR_NO_SPACE;
    }

    // Write entry to queue
    uint8_t *slot = get_slot(s_port->head);
    memcpy(slot, hdr, sizeof(*hdr));
    if (hdr->size > 0 && payload) {
        void *payload_ptr = get_slot_payload(slot);
        memcpy(payload_ptr, payload, hdr->size);
    }

    s_port->head = (s_port->head + 1) % s_port->queue_depth;
    s_port->count++;

    // Signal the dispatcher thread if running
    if (!s_port->sync_mode) {
        pthread_cond_signal(&s_port->cond);
    }

    pthread_mutex_unlock(&s_port->mutex);

    return BB_OK;
}

size_t bb_event_port_drain(uint32_t budget)
{
    if (!s_port) return 0;

    size_t dispatched = 0;
    uint32_t remaining = (budget == 0) ? ~0u : budget;

    while (remaining > 0) {
        pthread_mutex_lock(&s_port->mutex);

        if (s_port->count == 0) {
            pthread_mutex_unlock(&s_port->mutex);
            break;
        }

        // Pop one entry
        uint8_t *slot = get_slot(s_port->tail);
        bb_event_queue_entry_t entry;
        memcpy(&entry, slot, sizeof(entry));
        void *payload_ptr = get_slot_payload(slot);
        uint8_t payload[s_port->max_payload];
        if (entry.size > 0) {
            memcpy(payload, payload_ptr, entry.size);
        }

        s_port->tail = (s_port->tail + 1) % s_port->queue_depth;
        s_port->count--;

        pthread_mutex_unlock(&s_port->mutex);

        // Dispatch outside the lock
        bb_event_common_dispatch(&entry, payload);
        dispatched++;
        remaining--;
    }

    return dispatched;
}

void bb_event_port_lock(void)
{
    if (s_port) {
        pthread_mutex_lock(&s_port->mutex);
    }
}

void bb_event_port_unlock(void)
{
    if (s_port) {
        pthread_mutex_unlock(&s_port->mutex);
    }
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

#ifdef BB_EVENT_TESTING
void bb_event_port_set_malloc(void *(*m)(size_t)) { s_port_malloc = m ? m : malloc; }

void bb_event_port_reset_for_test(void) {
    s_test_last_drain_wait_ms = -1;
    /* free port resources if allocated; null out s_port so init can rerun */
    if (s_port) {
        if (s_port->thread_running) {
            /* NOTE: test_main.c's global setUp() calls bb_event_reset_for_test
             * (which clears every topic's subscriber list) BEFORE this
             * function, so a drain triggered here dispatches to whatever
             * subscribers the CURRENTLY-RUNNING test itself registered (or,
             * for the common case of a test that never subscribed, to no one
             * at all) -- not to the previous test's subscribers. Don't assume
             * a teardown-time drain proves delivery to a specific test's
             * subscriber; assert on subscribers registered by the SAME test
             * that posted the entry, before calling this function.
             *
             * Wait for the dispatcher thread to drain any entries a test
             * posted just before tearing down, using drained_cond -- a
             * dedicated condvar the dispatcher broadcasts on whenever count
             * hits 0 (bb_event_host.c's dispatcher_loop), NOT s_port->cond
             * (that one is producer -> dispatcher "queue has entries"; reusing
             * it here would mean two logically-different waiters -- the
             * dispatcher waiting for work and teardown waiting for drain --
             * racing the same predicate).
             *
             * Without this wait, teardown races the dispatcher: if it hasn't
             * yet been scheduled to pop the entry when thread_running flips
             * false below, it sees the flag and exits without dispatching --
             * silently dropping the entry and making dispatcher_loop's
             * pop/dispatch path (bb_event_host.c lines ~85-106) execute or
             * not purely on scheduling luck (this is what made its host
             * coverage flaky). Bounded (BB_EVENT_DRAIN_WAIT_TIMEOUT_MS) so a
             * genuinely stuck dispatcher can't hang CI -- if the deadline
             * passes with entries still queued, we fall through and tear the
             * port down anyway, which CAN still drop them (same class of
             * loss as the original bug, just gated behind a stuck-dispatcher
             * timeout instead of ordinary scheduling jitter); log it so that
             * loss is observable rather than silent. */
            pthread_mutex_lock(&s_port->mutex);
            if (s_port->count > 0) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                struct timespec deadline = compute_drain_deadline(now, BB_EVENT_DRAIN_WAIT_TIMEOUT_MS);
#ifdef BB_EVENT_TESTING
                struct timespec wait_start = now;
#endif
                while (s_port->count > 0) {
                    int rc = pthread_cond_timedwait(&s_port->drained_cond, &s_port->mutex, &deadline);
                    if (rc == ETIMEDOUT) break;
                }
#ifdef BB_EVENT_TESTING
                struct timespec wait_end;
                clock_gettime(CLOCK_REALTIME, &wait_end);
                s_test_last_drain_wait_ms =
                    (wait_end.tv_sec - wait_start.tv_sec) * 1000L
                    + (wait_end.tv_nsec - wait_start.tv_nsec) / 1000000L;
#endif
                if (s_port->count > 0) {
                    bb_log_w(TAG, "reset_for_test: dispatcher did not drain %zu pending "
                                  "entr%s before %dms timeout",
                             s_port->count, s_port->count == 1 ? "y" : "ies",
                             BB_EVENT_DRAIN_WAIT_TIMEOUT_MS);
                }
            }

            /* Signal the dispatcher thread to wake and exit, then join it
             * before destroying the mutex/condvar.  Destroying a condvar
             * while a thread is blocked in pthread_cond_wait is UB on Linux
             * (POSIX); the thread can block indefinitely, preventing process
             * exit and causing CI timeouts. */
            s_port->thread_running = false;
            pthread_cond_signal(&s_port->cond);
            pthread_mutex_unlock(&s_port->mutex);
            // drained_cond only proves the pop/decrement happened, not that
            // bb_event_common_dispatch() (which runs AFTER unlock, below)
            // has returned -- this join, not drained_cond, is what
            // guarantees dispatch has fully completed; do not remove it.
            pthread_join(s_port->dispatcher_thread, NULL);
        }
        pthread_cond_destroy(&s_port->drained_cond);
        pthread_cond_destroy(&s_port->cond);
        pthread_mutex_destroy(&s_port->mutex);
        free(s_port->queue_memory);
        free(s_port);
        s_port = NULL;
    }

    /* Never leak an armed startup delay into whatever test runs next.
     * Deliberately done LAST (after the pthread_join above, not at function
     * entry): a test can call bb_event_port_test_set_dispatcher_startup_delay_ms
     * then bb_event_init (spawning a dispatcher that hasn't necessarily been
     * scheduled by the OS yet) and immediately call this function to force
     * the drain-wait race -- zeroing the flag before that join would race
     * the not-yet-scheduled thread's own read of it, silently defeating the
     * delay. By the time we get here the dispatcher (if any) has already
     * exited and can no longer read this value. */
    s_test_dispatcher_startup_delay_ms = 0;
}
#endif
