// bb_bqueue — ESP-IDF backend: a thin wrap over a native FreeRTOS xQueue.
// Mailbox mode (capacity==1) uses xQueueOverwrite()/xQueueReset(); MPSC mode
// (capacity>1) uses plain xQueueSend()/xQueueReceive(). xQueuePeek() is
// non-consuming and safe for any number of concurrent callers per FreeRTOS's
// own contract — no broadcast-equivalent construction is needed here (unlike
// the host backend, which hand-builds blocking on bb_lock_cond and MUST use
// broadcast(); see platform/host/bb_bqueue/bb_bqueue.c). Deliberately does
// NOT consume bb_lock_cond (B1-822) — this wraps xQueue directly, so a
// subtly-wrong hand-rolled condvar-based design here could go unnoticed for
// a long time; xQueue is the proven primitive.
//
// Backed by a Kconfig-sized static instance pool (BB_BQUEUE_MAX_INSTANCES);
// zero heap — xQueueCreateStatic() with per-instance static storage sized to
// the Kconfig maxima (bb_bqueue_create() validates cfg against those maxima
// via the shared bb_bqueue_validate_cfg()).
#include "bb_bqueue.h"
#include "bb_bqueue_priv.h"
#include "bb_lock.h" // bb_lock_cond_ms_to_ticks() — canonical overflow-safe ms->ticks (never re-hand-rolled)

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdatomic.h>

typedef struct bb_bqueue {
    bool     in_use;
    size_t   capacity;
    size_t   item_bytes;

    QueueHandle_t queue;
    StaticQueue_t queue_storage;
    uint8_t  item_storage[BB_BQUEUE_MAX_CAPACITY * BB_BQUEUE_MAX_ITEM_BYTES];

    _Atomic size_t dropped; // MPSC only; concurrent producers may race the increment
} bb_bqueue_inst_t;

static bb_bqueue_inst_t s_pool[BB_BQUEUE_MAX_INSTANCES];

static bb_bqueue_inst_t *inst_from_handle(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = (bb_bqueue_inst_t *)q;
    if (!inst || !inst->in_use) return NULL;
    return inst;
}

static TickType_t bb_bqueue_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == BB_BQUEUE_WAIT_FOREVER) {
        return portMAX_DELAY;
    }
    // Never pdMS_TO_TICKS() directly here -- it overflows 32-bit TickType_t
    // arithmetic for a large timeout_ms; bb_lock_cond_ms_to_ticks() computes
    // the same ratio in a 64-bit intermediate and saturates instead of
    // wrapping (see bb_lock.h). max_ticks is clamped below portMAX_DELAY so
    // a saturated finite timeout can never be silently promoted to forever.
    return (TickType_t)bb_lock_cond_ms_to_ticks(timeout_ms, configTICK_RATE_HZ, portMAX_DELAY - 1);
}

bb_err_t bb_bqueue_create(const bb_bqueue_cfg_t *cfg, bb_bqueue_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    bb_err_t vrc = bb_bqueue_validate_cfg(cfg);
    if (vrc != BB_OK) return vrc;

    int idx = -1;
    for (int i = 0; i < BB_BQUEUE_MAX_INSTANCES; i++) {
        if (!s_pool[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return BB_ERR_NO_SPACE;

    bb_bqueue_inst_t *inst = &s_pool[idx];
    memset(inst, 0, sizeof(*inst));
    inst->capacity = cfg->capacity;
    inst->item_bytes = cfg->item_bytes;

    inst->queue = xQueueCreateStatic((UBaseType_t)cfg->capacity, (UBaseType_t)cfg->item_bytes,
                                      inst->item_storage, &inst->queue_storage);
    if (!inst->queue) {
        return BB_ERR_NO_MEM; // LCOV_EXCL_LINE — xQueueCreateStatic on caller-owned static storage cannot fail; defensive only, not host-buildable
    }

    inst->in_use = true;
    *out = (bb_bqueue_t)inst;
    return BB_OK;
}

void bb_bqueue_destroy(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst) return;

    if (inst->queue) {
        vQueueDelete(inst->queue);
    }
    memset(inst, 0, sizeof(*inst));
}

bb_err_t bb_bqueue_overwrite(bb_bqueue_t q, const void *item)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !item) return BB_ERR_INVALID_ARG;
    if (inst->capacity != 1) return BB_ERR_INVALID_STATE;

    xQueueOverwrite(inst->queue, item);
    return BB_OK;
}

bb_err_t bb_bqueue_reset(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->capacity != 1) return BB_ERR_INVALID_STATE;

    return (xQueueReset(inst->queue) == pdPASS) ? BB_OK : BB_ERR_INVALID_STATE; // LCOV_EXCL_BR_LINE — xQueueReset on a valid handle cannot fail; defensive only
}

bb_err_t bb_bqueue_send(bb_bqueue_t q, const void *item, uint32_t timeout_ms)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !item) return BB_ERR_INVALID_ARG;
    if (inst->capacity == 1) return BB_ERR_INVALID_STATE;

    if (xQueueSend(inst->queue, item, bb_bqueue_ticks(timeout_ms)) != pdTRUE) {
        atomic_fetch_add(&inst->dropped, 1);
        return (timeout_ms == 0) ? BB_ERR_NO_SPACE : BB_ERR_TIMEOUT;
    }
    return BB_OK;
}

bb_err_t bb_bqueue_dropped(bb_bqueue_t q, size_t *out_dropped)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !out_dropped) return BB_ERR_INVALID_ARG;
    if (inst->capacity == 1) return BB_ERR_INVALID_STATE;

    *out_dropped = atomic_load(&inst->dropped);
    return BB_OK;
}

bb_err_t bb_bqueue_peek(bb_bqueue_t q, void *out_item, uint32_t timeout_ms)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !out_item) return BB_ERR_INVALID_ARG;

    if (xQueuePeek(inst->queue, out_item, bb_bqueue_ticks(timeout_ms)) != pdTRUE) {
        return (timeout_ms == 0) ? BB_ERR_NOT_FOUND : BB_ERR_TIMEOUT;
    }
    return BB_OK;
}

bb_err_t bb_bqueue_receive(bb_bqueue_t q, void *out_item, uint32_t timeout_ms)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst || !out_item) return BB_ERR_INVALID_ARG;

    if (xQueueReceive(inst->queue, out_item, bb_bqueue_ticks(timeout_ms)) != pdTRUE) {
        return (timeout_ms == 0) ? BB_ERR_NOT_FOUND : BB_ERR_TIMEOUT;
    }
    return BB_OK;
}

size_t bb_bqueue_count(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    if (!inst) return 0;
    return (size_t)uxQueueMessagesWaiting(inst->queue);
}

size_t bb_bqueue_capacity(bb_bqueue_t q)
{
    bb_bqueue_inst_t *inst = inst_from_handle(q);
    return inst ? inst->capacity : 0;
}
