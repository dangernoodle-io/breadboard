#include "bb_event.h"
#include "bb_event_port.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bb_event_port";

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

typedef struct {
    QueueHandle_t queue;           // FreeRTOS queue of slot pointers
    QueueHandle_t free_list_queue; // Queue of free buffer slots
    SemaphoreHandle_t mutex;       // Recursive mutex for subscriber lists
    uint8_t *buffer_pool;          // Fixed-stride allocation: queue_depth slots
    size_t slot_stride;            // sizeof(entry) + max_payload
    size_t queue_depth;
    TaskHandle_t dispatcher_task;
} bb_event_port_state_t;

static bb_event_port_state_t s_port = {0};
static bool s_initialized = false;

// ---------------------------------------------------------------------------
// Dispatcher task
// ---------------------------------------------------------------------------

static void dispatcher_task(void *arg)
{
    bb_event_queue_entry_t *entry;

    while (1) {
        // Block on the dispatch queue
        if (xQueueReceive(s_port.queue, &entry, portMAX_DELAY) == pdTRUE) {
            if (entry) {
                // Payload is inline after the entry header
                const void *payload = (const uint8_t *)entry + sizeof(bb_event_queue_entry_t);

                // Dispatch to subscribers
                bb_event_common_dispatch(entry, payload);

                // Return slot to free list
                xQueueSend(s_port.free_list_queue, &entry, 0);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Port implementation
// ---------------------------------------------------------------------------

bb_err_t bb_event_port_init(size_t queue_depth, size_t max_payload,
                            size_t stack_size, int task_priority)
{
    if (s_initialized) return BB_OK;

    if (queue_depth == 0) queue_depth = 16;
    if (max_payload == 0) max_payload = 256;
    if (stack_size == 0) stack_size = 4096;
    if (task_priority == 0) task_priority = 5; // Default priority

    s_port.queue_depth = queue_depth;
    s_port.slot_stride = sizeof(bb_event_queue_entry_t) + max_payload;

    // Allocate fixed-stride buffer pool
    s_port.buffer_pool = heap_caps_calloc(queue_depth, s_port.slot_stride,
                                          MALLOC_CAP_DEFAULT | MALLOC_CAP_DMA);
    if (!s_port.buffer_pool) {
        bb_log_e(TAG, "failed to allocate buffer pool: %zu slots x %zu bytes",
                 queue_depth, s_port.slot_stride);
        return BB_ERR_NO_SPACE;
    }

    // Create dispatch queue (holds pointers to buffer slots)
    s_port.queue = xQueueCreate(queue_depth, sizeof(bb_event_queue_entry_t *));
    if (!s_port.queue) {
        bb_log_e(TAG, "failed to create dispatch queue");
        heap_caps_free(s_port.buffer_pool);
        return BB_ERR_NO_SPACE;
    }

    // Create free-list queue (recycled slots)
    s_port.free_list_queue = xQueueCreate(queue_depth, sizeof(bb_event_queue_entry_t *));
    if (!s_port.free_list_queue) {
        bb_log_e(TAG, "failed to create free-list queue");
        vQueueDelete(s_port.queue);
        heap_caps_free(s_port.buffer_pool);
        return BB_ERR_NO_SPACE;
    }

    // Populate free list with all buffer slots
    for (size_t i = 0; i < queue_depth; i++) {
        bb_event_queue_entry_t *slot = (bb_event_queue_entry_t *)
            (s_port.buffer_pool + (i * s_port.slot_stride));
        xQueueSend(s_port.free_list_queue, &slot, 0);
    }

    // Create recursive mutex
    s_port.mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_port.mutex) {
        bb_log_e(TAG, "failed to create recursive mutex");
        vQueueDelete(s_port.queue);
        vQueueDelete(s_port.free_list_queue);
        heap_caps_free(s_port.buffer_pool);
        return BB_ERR_NO_SPACE;
    }

    // Spawn dispatcher task
    BaseType_t ret = xTaskCreate(dispatcher_task, "bb_event_disp",
                                 stack_size / sizeof(StackType_t),
                                 NULL, task_priority, &s_port.dispatcher_task);
    if (ret != pdPASS) {
        bb_log_e(TAG, "failed to create dispatcher task");
        vSemaphoreDelete(s_port.mutex);
        vQueueDelete(s_port.queue);
        vQueueDelete(s_port.free_list_queue);
        heap_caps_free(s_port.buffer_pool);
        return BB_ERR_NO_SPACE;
    }

    s_initialized = true;
    bb_log_i(TAG, "initialized: queue_depth=%zu max_payload=%zu stack=%zu prio=%d",
             queue_depth, max_payload, stack_size, task_priority);
    return BB_OK;
}

bb_err_t bb_event_port_enqueue(const bb_event_queue_entry_t *hdr,
                               const void *payload)
{
    if (!hdr) return BB_ERR_INVALID_ARG;

    // Check if in ISR context
    if (xPortInIsrContext()) {
        bb_log_w(TAG, "enqueue not supported from ISR");
        return BB_ERR_INVALID_STATE;
    }

    // Grab a free slot from the pool
    bb_event_queue_entry_t *slot;
    if (xQueueReceive(s_port.free_list_queue, &slot, 0) != pdTRUE) {
        bb_log_w(TAG, "no free slots in pool (queue full)");
        return BB_ERR_NO_SPACE;
    }

    // Copy entry header
    memcpy(slot, hdr, sizeof(bb_event_queue_entry_t));

    // Copy payload inline (if provided)
    if (payload && hdr->size > 0) {
        void *payload_dst = (uint8_t *)slot + sizeof(bb_event_queue_entry_t);
        memcpy(payload_dst, payload, hdr->size);
    }

    // Post to dispatch queue
    if (xQueueSend(s_port.queue, &slot, 0) != pdTRUE) {
        bb_log_w(TAG, "failed to post to dispatch queue");
        xQueueSend(s_port.free_list_queue, &slot, 0);
        return BB_ERR_NO_SPACE;
    }

    return BB_OK;
}

size_t bb_event_port_drain(uint32_t budget)
{
    // ESP-IDF has a dedicated dispatcher task; cooperative drain is a no-op
    (void)budget;
    return 0;
}

void bb_event_port_lock(void)
{
    if (s_port.mutex) {
        xSemaphoreTakeRecursive(s_port.mutex, portMAX_DELAY);
    }
}

void bb_event_port_unlock(void)
{
    if (s_port.mutex) {
        xSemaphoreGiveRecursive(s_port.mutex);
    }
}
