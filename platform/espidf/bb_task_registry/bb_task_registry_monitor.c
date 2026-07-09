// bb_task_registry software watchdog monitor task (B1-458 PR-B).
//
// bb_task_registry_sw_wdt_start() is invoked via the bbtool:init codegen
// marker (see bb_task_registry.h) — gated by CONFIG_BB_TASK_REGISTRY_SW_WDT
// (default n; a no-op stub compiles in when off). The evaluator API
// (bb_task_registry_sw_wdt_check) and the
// opts->sw_wdt_timeout_ms field compile in unconditionally on host + ESP-IDF
// so tests always see them; this file gates only the monitor TASK.
//
// The monitor registers ITSELF into bb_task_registry (so it shows up in
// GET /api/diag/tasks like every other tracked task) from INSIDE its own
// task body, before entering its loop — the token stays a plain local
// variable, never published across tasks, so no atomic/acquire-release
// discipline is needed here (contrast bb_timer.c, which does need it because
// its token/handle crosses task boundaries).
#include "sdkconfig.h"
#include "bb_task_registry.h"

#if CONFIG_BB_TASK_REGISTRY_SW_WDT

#include "bb_log.h"
#include "bb_clock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb_task_registry_sw_wdt";

// Kconfig bridge (see CLAUDE.md "Avoiding audit-class regressions").
#ifdef CONFIG_BB_TASK_REGISTRY_SW_WDT_CHECK_MS
#define BB_TASK_REGISTRY_SW_WDT_CHECK_MS CONFIG_BB_TASK_REGISTRY_SW_WDT_CHECK_MS
#endif
#ifndef BB_TASK_REGISTRY_SW_WDT_CHECK_MS
#define BB_TASK_REGISTRY_SW_WDT_CHECK_MS 1000
#endif

#ifdef CONFIG_BB_TASK_REGISTRY_SW_WDT_STACK
#define BB_TASK_REGISTRY_SW_WDT_STACK CONFIG_BB_TASK_REGISTRY_SW_WDT_STACK
#endif
#ifndef BB_TASK_REGISTRY_SW_WDT_STACK
#define BB_TASK_REGISTRY_SW_WDT_STACK 3072
#endif

#ifdef CONFIG_BB_TASK_REGISTRY_SW_WDT_PRIORITY
#define BB_TASK_REGISTRY_SW_WDT_PRIORITY CONFIG_BB_TASK_REGISTRY_SW_WDT_PRIORITY
#endif
#ifndef BB_TASK_REGISTRY_SW_WDT_PRIORITY
#define BB_TASK_REGISTRY_SW_WDT_PRIORITY 2
#endif

#ifdef CONFIG_BB_TASK_REGISTRY_SW_WDT_CORE
#define BB_TASK_REGISTRY_SW_WDT_CORE CONFIG_BB_TASK_REGISTRY_SW_WDT_CORE
#endif
#ifndef BB_TASK_REGISTRY_SW_WDT_CORE
#define BB_TASK_REGISTRY_SW_WDT_CORE (-1)
#endif

#ifdef CONFIG_BB_TASK_REGISTRY_SW_WDT_HW_SUBSCRIBE
#define BB_TASK_REGISTRY_SW_WDT_HW_SUBSCRIBE CONFIG_BB_TASK_REGISTRY_SW_WDT_HW_SUBSCRIBE
#endif
#ifndef BB_TASK_REGISTRY_SW_WDT_HW_SUBSCRIBE
#define BB_TASK_REGISTRY_SW_WDT_HW_SUBSCRIBE 0
#endif

static void sw_wdt_monitor_task(void *arg)
{
    (void)arg;

    // Self-register with sw_wdt_timeout_ms=0 — the monitor watches OTHER
    // tasks, not itself; opts->hw_wdt_subscribe wires it into the hardware
    // Task WDT so a hung/dead monitor still trips the global TWDT.
    bb_task_registry_opts_t opts = {
        .hw_wdt_subscribe  = BB_TASK_REGISTRY_SW_WDT_HW_SUBSCRIBE,
        .sw_wdt_timeout_ms = 0,
    };
    bb_task_registry_token_t token = BB_TASK_REGISTRY_TOKEN_INVALID;
    bb_err_t err = bb_task_registry_register("bb_sw_wdt", BB_TASK_REGISTRY_SW_WDT_STACK,
                                              xTaskGetCurrentTaskHandle(), &opts, &token);
    if (err != BB_OK) {
        bb_log_w(TAG, "self-register failed: %d", (int)err);
    }

    bb_log_i(TAG, "monitor started; check every %d ms", BB_TASK_REGISTRY_SW_WDT_CHECK_MS);

    for (;;) {
        bb_task_registry_sw_wdt_check(bb_clock_now_ms());
        bb_task_registry_feed(token);
        vTaskDelay(pdMS_TO_TICKS(BB_TASK_REGISTRY_SW_WDT_CHECK_MS));
    }
}

bb_err_t bb_task_registry_sw_wdt_start(void)
{
    // NOT migrated to bb_task_create() (Option C, B1-690): this task
    // self-registers via bb_task_registry_register() with a non-default
    // bb_task_registry_opts_t (hw_wdt_subscribe) that bb_task_config_t has
    // no field for. Stays on raw xTaskCreate[PinnedToCore] pending an opts
    // pass-through into bb_task; no functional change here.
    //
    // On single-core (unicore) targets, core 1 does not exist and
    // xTaskCreatePinnedToCore asserts; fall back to no affinity. Mirrors
    // bb_ota_boot's status_task_core guard.
    int core = BB_TASK_REGISTRY_SW_WDT_CORE;
    if (core >= configNUMBER_OF_CORES) {
        core = tskNO_AFFINITY;
    }

    TaskHandle_t task = NULL;
    BaseType_t ok;
    if (core < 0) {
        ok = xTaskCreate(sw_wdt_monitor_task, "bb_sw_wdt", BB_TASK_REGISTRY_SW_WDT_STACK, NULL,
                          BB_TASK_REGISTRY_SW_WDT_PRIORITY, &task);
    } else {
        ok = xTaskCreatePinnedToCore(sw_wdt_monitor_task, "bb_sw_wdt", BB_TASK_REGISTRY_SW_WDT_STACK,
                                      NULL, BB_TASK_REGISTRY_SW_WDT_PRIORITY, &task, core);
    }
    if (ok != pdPASS) {
        bb_log_e(TAG, "monitor task create failed");
        return BB_ERR_NO_MEM;
    }
    return BB_OK;
}

#else /* CONFIG_BB_TASK_REGISTRY_SW_WDT not set */

bb_err_t bb_task_registry_sw_wdt_start(void)
{
    return BB_OK;
}

#endif  // CONFIG_BB_TASK_REGISTRY_SW_WDT
