// Arduino stub for bb_bqueue — returns BB_ERR_UNSUPPORTED. Arduino targets
// in this workspace are AVR / Cortex-M class with no shared cross-thread
// blocking-queue primitive; bb_bqueue's sole consumer (TaipanMiner, TA-562)
// is ESP-IDF only. Implement per-board if a real Arduino consumer appears.
// Mirrors platform/arduino/bb_tcp_client's stub precedent.
#include "bb_bqueue.h"

bb_err_t bb_bqueue_create(const bb_bqueue_cfg_t *cfg, bb_bqueue_t *out)
{
    (void)cfg;
    if (out) *out = NULL;
    return BB_ERR_UNSUPPORTED;
}

void bb_bqueue_destroy(bb_bqueue_t q)
{
    (void)q;
}

bb_err_t bb_bqueue_overwrite(bb_bqueue_t q, const void *item)
{
    (void)q; (void)item;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_bqueue_reset(bb_bqueue_t q)
{
    (void)q;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_bqueue_send(bb_bqueue_t q, const void *item, uint32_t timeout_ms)
{
    (void)q; (void)item; (void)timeout_ms;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_bqueue_dropped(bb_bqueue_t q, size_t *out_dropped)
{
    (void)q;
    if (out_dropped) *out_dropped = 0;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_bqueue_peek(bb_bqueue_t q, void *out_item, uint32_t timeout_ms)
{
    (void)q; (void)out_item; (void)timeout_ms;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_bqueue_receive(bb_bqueue_t q, void *out_item, uint32_t timeout_ms)
{
    (void)q; (void)out_item; (void)timeout_ms;
    return BB_ERR_UNSUPPORTED;
}

size_t bb_bqueue_count(bb_bqueue_t q)
{
    (void)q;
    return 0;
}

size_t bb_bqueue_capacity(bb_bqueue_t q)
{
    (void)q;
    return 0;
}
