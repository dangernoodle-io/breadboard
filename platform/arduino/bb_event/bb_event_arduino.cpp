#include "bb_event_port.h"
#include "bb_log.h"
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_event_arduino";

// Ring queue state
typedef struct {
    uint8_t *buffer;          // malloc'd: (queue_depth * slot_size) bytes
    size_t queue_depth;
    size_t max_payload;
    size_t slot_size;         // sizeof(bb_event_queue_entry_t) + max_payload
    volatile size_t head;     // write index (next enqueue slot)
    volatile size_t tail;     // read index (next dispatch slot)
    volatile size_t count;    // # of queued events
} bb_event_port_state_t;

static bb_event_port_state_t s_state = {0};
static bool s_initialized = false;

// ============================================================================
// Critical section helpers (ISR-safe on all Arduino targets)
// ============================================================================

// Local lock for subscriber list mutation (not the ring queue, which uses
// noInterrupts/interrupts for index updates only).
static volatile uint8_t s_locked = 0;

static inline void lock_subscribers(void)
{
    // Spin with interrupts disabled to ensure mutual exclusion.
    // This is intentionally simple: subscribers list is only touched from
    // bb_event_subscribe/unsubscribe (non-ISR) and during dispatch from bb_event_pump.
    // Neither is hot path; brief spin is acceptable.
    while (1) {
        noInterrupts();
        if (!s_locked) {
            s_locked = 1;
            interrupts();
            return;
        }
        interrupts();
    }
}

static inline void unlock_subscribers(void)
{
    noInterrupts();
    s_locked = 0;
    interrupts();
}

// ============================================================================
// Port implementation
// ============================================================================

bb_err_t bb_event_port_init(size_t queue_depth, size_t max_payload,
                            size_t stack_size, int task_priority)
{
    (void)stack_size;     // unused on Arduino
    (void)task_priority;  // unused on Arduino

    if (s_initialized) return BB_OK;
    if (queue_depth == 0 || max_payload == 0) return BB_ERR_INVALID_ARG;

    s_state.queue_depth = queue_depth;
    s_state.max_payload = max_payload;
    s_state.slot_size = sizeof(bb_event_queue_entry_t) + max_payload;

#ifdef BB_EVENT_ARDUINO_NO_HEAP
    // Static buffer fallback: compile-time max_payload from Kconfig
    // For now, default to malloc — TODO: implement Kconfig-based static buffer if needed
    // For AVR targets with severe RAM constraints, add compile-time conditional allocation here.
    bb_log_w(TAG, "BB_EVENT_ARDUINO_NO_HEAP defined but static buffer not yet implemented; using malloc");
#endif

    s_state.buffer = (uint8_t *)malloc(queue_depth * s_state.slot_size);
    if (!s_state.buffer) {
        bb_log_e(TAG, "failed to allocate queue buffer: depth=%zu slot_size=%zu",
                 queue_depth, s_state.slot_size);
        return BB_ERR_NO_SPACE;
    }

    s_state.head = 0;
    s_state.tail = 0;
    s_state.count = 0;

    s_initialized = true;
    bb_log_i(TAG, "initialized: queue_depth=%zu max_payload=%zu",
             queue_depth, max_payload);
    return BB_OK;
}

bb_err_t bb_event_port_enqueue(const bb_event_queue_entry_t *hdr,
                               const void *payload)
{
    if (!hdr) return BB_ERR_INVALID_ARG;
    if (!s_initialized) return BB_ERR_INVALID_STATE;

    // Queue is full?
    if (s_state.count >= s_state.queue_depth) {
        bb_log_w(TAG, "queue full: %zu/%zu", s_state.count, s_state.queue_depth);
        return BB_ERR_NO_SPACE;
    }

    // Copy entry header and payload into the slot.
    // This happens outside the critical section to minimize ISR latency.
    uint8_t *slot = s_state.buffer + (s_state.head * s_state.slot_size);
    memcpy(slot, hdr, sizeof(bb_event_queue_entry_t));
    if (payload && hdr->size > 0) {
        memcpy(slot + sizeof(bb_event_queue_entry_t), payload, hdr->size);
    }

    // Update head index under interrupt protection.
    noInterrupts();
    s_state.head = (s_state.head + 1) % s_state.queue_depth;
    s_state.count++;
    interrupts();

    return BB_OK;
}

size_t bb_event_port_drain(uint32_t budget)
{
    if (!s_initialized) return 0;

    size_t dispatched = 0;

    // Drain up to 'budget' events (0 = all).
    while ((budget == 0 || dispatched < budget) && s_state.count > 0) {
        // Dequeue under lock.
        noInterrupts();
        if (s_state.count == 0) {
            interrupts();
            break;
        }
        size_t tail_idx = s_state.tail;
        s_state.tail = (s_state.tail + 1) % s_state.queue_depth;
        s_state.count--;
        interrupts();

        // Dispatch outside lock.
        uint8_t *slot = s_state.buffer + (tail_idx * s_state.slot_size);
        const bb_event_queue_entry_t *entry = (const bb_event_queue_entry_t *)slot;
        const void *payload = slot + sizeof(bb_event_queue_entry_t);

        bb_event_common_dispatch(entry, payload);
        dispatched++;
    }

    return dispatched;
}

void bb_event_port_lock(void)
{
    lock_subscribers();
}

void bb_event_port_unlock(void)
{
    unlock_subscribers();
}
