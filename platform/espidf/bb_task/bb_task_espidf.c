// bb_task — ESP-IDF creation shell: bb_task_resolve() -> the matching
// xTaskCreate* variant -> bb_task_base_upsert(). Coverage-ungated (thin
// FreeRTOS glue only; the resolver + base ops it calls are the
// coverage-gated pure code in components/bb_task/src/bb_task_common.c).
#include "bb_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bb_err_t bb_task_create(const bb_task_config_t *cfg, void **out_handle)
{
    if (out_handle) {
        *out_handle = NULL;
    }
    if (!cfg) {
        return BB_ERR_INVALID_ARG;
    }

    bb_task_resolved_t resolved;
    bb_err_t err = bb_task_resolve(cfg, configNUMBER_OF_CORES, &resolved);
    if (err != BB_OK) {
        return err;
    }

    TaskHandle_t handle = NULL;
    BaseType_t   ok;

    // xTaskCreate*'s depth arg is in StackType_t units, not bytes -- on this
    // kernel (CONFIG_FREERTOS_SMP=n) StackType_t is uint8_t so depth==bytes,
    // but computing it via sizeof(StackType_t) keeps this correct on any
    // kernel/target combination without a hand-rolled constant (mirrors
    // platform/espidf/bb_event_routes/bb_event_routes_espidf.c's
    // SSE_TASK_STACK_WORDS idiom). The STATIC stack_buf element count must
    // agree with this same depth -- callers size their StackType_t
    // stack_buf[] arrays to hold `depth` elements (== stack_bytes bytes on
    // this port).
    uint32_t depth = resolved.stack_bytes / sizeof(StackType_t);

    if (resolved.backing == BB_TASK_BACKING_STATIC) {
        StackType_t  *stack = (StackType_t *)cfg->stack_buf;
        StaticTask_t *tcb   = (StaticTask_t *)cfg->tcb_buf;
        if (resolved.core == BB_TASK_CORE_ANY) {
            handle = xTaskCreateStatic((TaskFunction_t)cfg->entry, cfg->name,
                                       depth, cfg->arg,
                                       (UBaseType_t)cfg->priority, stack, tcb);
        } else {
            handle = xTaskCreateStaticPinnedToCore((TaskFunction_t)cfg->entry, cfg->name,
                                                    depth, cfg->arg,
                                                    (UBaseType_t)cfg->priority, stack, tcb,
                                                    resolved.core);
        }
        ok = (handle != NULL) ? pdPASS : pdFAIL;
    } else if (resolved.core == BB_TASK_CORE_ANY) {
        ok = xTaskCreate((TaskFunction_t)cfg->entry, cfg->name, depth,
                          cfg->arg, (UBaseType_t)cfg->priority, &handle);
    } else {
        ok = xTaskCreatePinnedToCore((TaskFunction_t)cfg->entry, cfg->name, depth,
                                      cfg->arg, (UBaseType_t)cfg->priority, &handle,
                                      resolved.core);
    }

    if (ok != pdPASS || !handle) {
        // Silent -- bb_task is a floor-safe primitive with no bb_log
        // dependency (would form a component cycle: bb_log's writer task
        // creates via bb_task_create()). Caller already gets BB_ERR_NO_MEM.
        return BB_ERR_NO_MEM;
    }

    // Base registry records bytes (platform-uniform diagnostics unit); the
    // xTaskCreate* depth argument above is words (StackType_t units) -- keep
    // these two locals distinct rather than passing `depth` into the
    // registry, which would ship a cross-platform unit split with host
    // (see platform/host/bb_task/bb_task_host.c, which already passes bytes).
    // Base upsert failure is silent for the same reason (no bb_log dep) --
    // best-effort diagnostics only, task creation itself already succeeded.
    (void)bb_task_base_upsert(handle, cfg->name, resolved.stack_bytes, cfg->wdt_arm);

    if (out_handle) {
        *out_handle = handle;
    }
    return BB_OK;
}
