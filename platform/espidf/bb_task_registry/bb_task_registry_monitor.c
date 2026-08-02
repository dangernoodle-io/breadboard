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
#include "bb_task.h"

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
        bb_task_delay_ms(BB_TASK_REGISTRY_SW_WDT_CHECK_MS);
    }
}

bb_err_t bb_task_registry_sw_wdt_start(void)
{
    // Migrated to bb_task_create() (B1-1364 PR3): routes affinity resolution
    // through bb_task_resolve(), so this monitor is now steerable away from
    // a core another task claims via bb_wdt's core-claim mechanism (B1-1364
    // PR2). core_owning is deliberately false -- the monitor does not own a
    // core, it is a task that should be steered away from one.
    //
    // Behaviour is unchanged when no core is claimed: same name/stack/
    // priority, and bb_task_resolve()'s unicore clamp (core >= num_cores ->
    // BB_TASK_CORE_ANY) reproduces the former manual
    // `core >= configNUMBER_OF_CORES -> tskNO_AFFINITY` guard exactly (both
    // leave a negative/BB_TASK_CORE_ANY core untouched). BB_TASK_CORE_ANY
    // (-1) and tskNO_AFFINITY are the same steering target either way.
    //
    // The self-registration into bb_task_registry below (sw_wdt_monitor_task)
    // is a separate, deliberately NOT-migrated bootstrap exception (B1-1375)
    // -- see this file's header comment.
    bb_task_config_t task_cfg = {
        .entry       = sw_wdt_monitor_task,
        .name        = "bb_sw_wdt",
        .arg         = NULL,
        .stack_bytes = BB_TASK_REGISTRY_SW_WDT_STACK,
        .priority    = BB_TASK_REGISTRY_SW_WDT_PRIORITY,
        .core        = BB_TASK_REGISTRY_SW_WDT_CORE,
        .core_owning = false,
        .backing     = BB_TASK_BACKING_DYNAMIC,
    };

    void *handle = NULL;
    bb_err_t err = bb_task_create(&task_cfg, &handle);
    if (err != BB_OK) {
        bb_log_e(TAG, "monitor task create failed: %d", (int)err);
        return err;
    }
    return BB_OK;
}

#else /* CONFIG_BB_TASK_REGISTRY_SW_WDT not set */

bb_err_t bb_task_registry_sw_wdt_start(void)
{
    return BB_OK;
}

#endif  // CONFIG_BB_TASK_REGISTRY_SW_WDT
