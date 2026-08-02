// smoke_core_claim -- synthetic core-claimer proof harness (B1-1364 PR5
// validation, examples/smoke ONLY -- never a library component; this file
// exists solely to make the httpd-worker-core steering decision
// (platform/espidf/bb_http_server/bb_http.c, bb_http_core_steer_resolve())
// actually execute end-to-end on real hardware/CI, instead of remaining a
// dormant decision matrix exercised only by host tests. Nothing in the
// tree calls bb_wdt_claim_core_exclusive() before httpd starts otherwise.
//
// Gated by CONFIG_BB_SMOKE_CORE_CLAIM (default n) -- both entry points below
// compile in UNCONDITIONALLY (as no-op BB_OK stubs when the knob is off) so
// this .c file always links, regardless of Kconfig: codegen greps this
// file's owning markers (examples/smoke/main/bb_wire.h) unconditionally at
// grep time, so the symbols must exist on every smoke build. Mirrors
// platform/espidf/bb_task_registry/bb_task_registry_monitor.c's own
// CONFIG_BB_TASK_REGISTRY_SW_WDT gate shape.
#include "sdkconfig.h"
#include "smoke_core_claim.h"

#if CONFIG_BB_SMOKE_CORE_CLAIM

#include "bb_task.h"
#include "bb_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "smoke_core_claim";

// Kconfig bridge (see CLAUDE.md "Avoiding audit-class regressions").
#ifdef CONFIG_BB_SMOKE_CORE_CLAIM_CORE
#define SMOKE_CORE_CLAIM_CORE CONFIG_BB_SMOKE_CORE_CLAIM_CORE
#endif
#ifndef SMOKE_CORE_CLAIM_CORE
#define SMOKE_CORE_CLAIM_CORE 1
#endif

#define SMOKE_CORE_CLAIM_STACK 3072
// Above HTTPD_DEFAULT_CONFIG()'s task_priority (tskIDLE_PRIORITY+5, see
// esp_http_server.h) -- a real pinned owner (e.g. an ASIC-mining task) runs
// at a priority that will not be starved by httpd's own worker, and this
// task should represent that same shape.
#define SMOKE_CORE_CLAIM_PRIORITY 10

// WDT-safety rationale: this task deliberately never subscribes to the
// hardware Task WDT (bb_wdt_task_subscribe*/esp_task_wdt_add), so it cannot
// trip it directly. The mechanism under test is also why it cannot trip it
// INDIRECTLY through the claimed core's idle task: bb_task_create()'s
// core_owning=true path calls bb_wdt_claim_core_exclusive()
// (components/bb_wdt) BEFORE this task is created, which folds
// SMOKE_CORE_CLAIM_CORE into the Task WDT's excused idle-check mask on the
// very next reconfigure -- exactly the mechanism a real pinned owner relies
// on (see bb_wdt.h's "core-claim mechanism" doc).
//
// What this task still must not do is starve its claimed core FOREVER: an
// unbounded `for(;;){}` at a real priority would prevent every
// lower-priority (and, if pinned onto a core WiFi/lwIP also uses,
// networking) task on that core from ever running again -- including this
// task's own boot-time peers, which could hang the proof before its log
// line ever runs. So the loop below spins a bounded, CPU-heavy burst (a
// few million iterations, no I/O, no yield point) and then explicitly
// yields via bb_task_delay_ms(1) -- a high duty cycle (~98%+), genuinely
// representative of KB 350's "CPU-bound pinned task" contention shape,
// without permanently starving the core it claims. bb_task_delay_ms(1) is
// used (not bb_task_yield()) specifically because it is a REAL block, not a
// same-priority-only yield -- it lets strictly lower-priority tasks on this
// core (not just equal-priority ones) get scheduled too.
static void core_claim_busy_task(void *arg)
{
    (void)arg;
    volatile uint32_t spin = 0;
    for (;;) {
        for (uint32_t i = 0; i < 2000000; i++) {
            spin += i;
        }
        bb_task_delay_ms(1);
    }
}

bb_err_t smoke_core_claim_start(void)
{
    bb_task_config_t cfg = {
        .entry       = core_claim_busy_task,
        .name        = "smoke_claim",
        .arg         = NULL,
        .stack_bytes = SMOKE_CORE_CLAIM_STACK,
        .priority    = SMOKE_CORE_CLAIM_PRIORITY,
        .core        = SMOKE_CORE_CLAIM_CORE,
        .core_owning = true,
        .backing     = BB_TASK_BACKING_DYNAMIC,
    };
    void *handle = NULL;
    bb_err_t err = bb_task_create(&cfg, &handle);
    if (err != BB_OK) {
        bb_log_e(TAG, "core-claim task create failed: %d", (int)err);
        return err;
    }
    bb_log_i(TAG, "claimed core %d exclusively before httpd start", SMOKE_CORE_CLAIM_CORE);
    return BB_OK;
}

bb_err_t smoke_core_claim_log_httpd_core(void)
{
    // esp_http_server names its worker task "httpd" unconditionally
    // (esp_http_server/src/httpd_main.c) -- not exposed via any public
    // accessor, so this looks it up by name via the standard FreeRTOS
    // xTaskGetHandle() (INCLUDE_xTaskGetHandle=1 in this repo's
    // FreeRTOSConfig.h). xTaskGetCoreID() then reports the pinned core
    // httpd_config_t.core_id actually resolved to -- the direct, on-target
    // observable of the steering decision under test.
    TaskHandle_t httpd_task = xTaskGetHandle("httpd");
    if (!httpd_task) {
        bb_log_w(TAG, "httpd task handle not found -- server not started?");
        return BB_OK;
    }
    BaseType_t core = xTaskGetCoreID(httpd_task);
    if (core == tskNO_AFFINITY) {
        // This log line is the on-serial observable the proof reads: with
        // the claimer disabled (or nothing claimed), httpd is unsteered
        // and this "no affinity" line is what should print.
        bb_log_i(TAG, "httpd worker core: no affinity");
    } else {
        bb_log_i(TAG, "httpd worker core: %d", (int)core);
    }
    return BB_OK;
}

#else /* CONFIG_BB_SMOKE_CORE_CLAIM not set */

bb_err_t smoke_core_claim_start(void)
{
    return BB_OK;
}

bb_err_t smoke_core_claim_log_httpd_core(void)
{
    return BB_OK;
}

#endif
