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

static const char *TAG = "bb_event_host";

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
    pthread_cond_t cond;
    pthread_t dispatcher_thread;
    bool thread_running;
    bool sync_mode;               // BB_EVENT_HOST_SYNC=1: skip thread, use bb_event_port_drain synchronously
};

static bb_event_port_t *s_port = NULL;

#ifdef BB_EVENT_TESTING
static void *(*s_port_malloc)(size_t) = malloc;
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

    // Initialize condition variable
    pthread_cond_init(&s_port->cond, NULL);

    // Spawn dispatcher thread (unless in sync mode)
    if (!s_port->sync_mode) {
        s_port->thread_running = true;
        int ret = pthread_create(&s_port->dispatcher_thread, NULL, dispatcher_loop, NULL);
        if (ret != 0) {
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
    /* free port resources if allocated; null out s_port so init can rerun */
    if (s_port) {
        if (s_port->thread_running) {
            /* Signal the dispatcher thread to wake and exit, then join it
             * before destroying the mutex/condvar.  Destroying a condvar
             * while a thread is blocked in pthread_cond_wait is UB on Linux
             * (POSIX); the thread can block indefinitely, preventing process
             * exit and causing CI timeouts. */
            pthread_mutex_lock(&s_port->mutex);
            s_port->thread_running = false;
            pthread_cond_signal(&s_port->cond);
            pthread_mutex_unlock(&s_port->mutex);
            pthread_join(s_port->dispatcher_thread, NULL);
        }
        pthread_cond_destroy(&s_port->cond);
        pthread_mutex_destroy(&s_port->mutex);
        free(s_port->queue_memory);
        free(s_port);
        s_port = NULL;
    }
}
#endif
