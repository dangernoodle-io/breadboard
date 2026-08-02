// bb_task — ESP-IDF creation shell: bb_task_resolve() -> the matching
// xTaskCreate* variant -> bb_task_base_upsert(). Also the espidf shell for
// bb_task_delay_ms()/bb_task_yield() (vTaskDelay()/taskYIELD() thin
// wrappers). Coverage-ungated (thin FreeRTOS glue only; the resolver +
// tick-conversion + base ops it calls are the coverage-gated pure code in
// components/bb_task/src/bb_task_common.c).
#include "bb_task.h"
#include "bb_num.h"
#include "bb_wdt.h"

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
    // bb_wdt_claimed_core_mask() is read here (the platform shell), not
    // inside bb_task_resolve() -- keeps the resolver a pure function of its
    // arguments (B1-1356). See bb_task.h's bb_task_resolve() doc.
    bb_err_t err = bb_task_resolve(cfg, configNUMBER_OF_CORES,
                                    bb_wdt_claimed_core_mask(), &resolved);
    if (err != BB_OK) {
        return err;
    }

    // core_owning claim (B1-1364 PR2/PR3-fix): only a REAL resolved core is
    // ever claimed -- bb_task_resolve()'s unicore clamp already degrades an
    // out-of-range owning request to BB_TASK_CORE_ANY, so this never calls
    // bb_wdt_claim_core_exclusive() with a core that doesn't exist. Claimed
    // BEFORE the task is created, so the Task WDT idle-check excuse is in
    // effect before the pinned task can starve that core's idle task (see
    // bb_wdt.h's core-claim ordering requirement).
    //
    // bb_wdt_claim_core_exclusive() (not the plain, permissive
    // bb_wdt_claim_core()) is the actual exclusivity enforcement -- an
    // atomic check-and-set under bb_wdt's own lock, closing the TOCTOU
    // window between bb_task_resolve()'s claimed_core_mask snapshot (read
    // above, before resolve) and this call: two concurrent core_owning
    // creations targeting the same core can both pass that snapshot check,
    // but only one of them wins this call. The other propagates
    // BB_ERR_INVALID_STATE out of bb_task_create() without creating a task.
    bool owning_claim = cfg->core_owning && resolved.core != BB_TASK_CORE_ANY;
    if (owning_claim) {
        bb_err_t claim_rc = bb_wdt_claim_core_exclusive(resolved.core);
        if (claim_rc != BB_OK) {
            return claim_rc;
        }
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
        // Task creation failed after a successful claim above -- release it
        // rather than leaking an excused-idle-check claim for a task that
        // was never created.
        if (owning_claim) {
            bb_wdt_release_core(resolved.core);
        }
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
    // Base upsert failure (e.g. BB_ERR_NO_SPACE, BB_TASK_BASE_MAX
    // exhausted) is otherwise silent for the same reason (no bb_log dep) --
    // best-effort diagnostics only, task creation itself already succeeded.
    bb_err_t base_rc = bb_task_base_upsert(handle, cfg->name, resolved.stack_bytes, cfg->wdt_arm);
    // Records what was actually claimed above (owning_claim), not just
    // cfg->core_owning, so bb_task_deregister() releases exactly what was
    // claimed -- including the unicore no-op-claim case, where owning_claim
    // is false even though cfg->core_owning was requested true.
    if (base_rc == BB_OK) {
        base_rc = bb_task_base_set_core_owning(handle, resolved.core, owning_claim);
    }
    if (owning_claim && base_rc != BB_OK) {
        // The base registry could not record this handle (or its
        // core-owning fields), so bb_task_deregister() will never find it
        // to release the claim taken above (it looks the handle up in this
        // same registry and returns BB_ERR_NOT_FOUND when absent) --
        // release it here instead, or it leaks for the life of the device,
        // permanently excusing this core's idle check.
        bb_wdt_release_core(resolved.core);
    }

    if (out_handle) {
        *out_handle = handle;
    }
    return BB_OK;
}

void bb_task_delay_ms(uint32_t ms)
{
    uint32_t ticks = bb_task_delay_ticks_for_ms(ms, configTICK_RATE_HZ);
    vTaskDelay(ticks);
}

void bb_task_yield(void)
{
    taskYIELD();
}

// bb_task_stack_hwm_bytes -- LIVE PULL, no Kconfig guard needed. See
// bb_task.h's doc: uxTaskGetStackHighWaterMark() depends only on
// INCLUDE_uxTaskGetStackHighWaterMark, which this repo's FreeRTOSConfig.h
// hardcodes to 1 unconditionally -- unlike CONFIG_FREERTOS_USE_TRACE_
// FACILITY, which gates the DIFFERENT uxTaskGetSystemState() API the base
// registry's periodic scan (bb_task_registry_base_scan.c) uses.
uint32_t bb_task_stack_hwm_bytes(void *handle_or_null)
{
    UBaseType_t words = uxTaskGetStackHighWaterMark((TaskHandle_t)handle_or_null);
    // uxTaskGetStackHighWaterMark() returns WORDS, not bytes -- convert via
    // the shared SSOT (B1-1256), same helper/word-size argument
    // bb_task_registry_base_scan.c's periodic scan already uses for this
    // identical conversion. Do not hand-roll a second
    // `* sizeof(StackType_t)` multiply.
    return (uint32_t)bb_num_words_to_bytes((uint32_t)words, sizeof(StackType_t));
}
