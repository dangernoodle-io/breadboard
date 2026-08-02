#include "bb_wdt.h"
#include "bb_wdt_priv.h"

#include "bb_lock.h"
#include "bb_lock_once.h"
#include "bb_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "bb_wdt";

// Last timeout applied via bb_wdt_set_timeout(); bb_wdt_priv_reapply()
// reissues a reconfigure at this timeout with a freshly derived idle mask
// (e.g. after bb_wdt_claim_core()/bb_wdt_release_core() change the claimed
// mask), so a claim can never be silently clobbered by a later reconfigure.
// Read/written under s_timeout_lock: bb_wdt_set_timeout() and
// bb_wdt_priv_reapply() (called from bb_wdt_claim_core()/
// bb_wdt_release_core()) can run on different tasks once composition wires
// claims into task creation, so this can no longer be a plain unlocked
// global (C11 data race otherwise).
static uint32_t s_last_timeout_s = CONFIG_ESP_TASK_WDT_TIMEOUT_S;

static bb_once_t s_timeout_lock_once = BB_ONCE_INIT;
static bb_lock_t s_timeout_lock;

// Lazily bb_lock_init() s_timeout_lock exactly once -- mirrors
// bb_wdt_core_claim.c's identical ensure_lock() idiom for s_claimed_mask.
static inline bb_err_t ensure_timeout_lock(void)
{
    bb_lock_config_t cfg = { .name = "bb_wdt_timeout" };
    return bb_lock_once_ensure(&s_timeout_lock_once, &cfg, &s_timeout_lock);
}

// The board's Kconfig-configured idle-check default, independent of any
// runtime claim.
static uint32_t kconfig_default_idle_mask(void)
{
    return
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
        (1U << 0) |
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
        (1U << 1) |
#endif
        0U;
}

// SHARED reconfigure path -- both bb_wdt_set_timeout() and
// bb_wdt_priv_reapply() (claim/release) go through this single function, so
// every reconfigure derives the idle mask via the same
// bb_wdt_derive_idle_mask() union (kconfig default | claimed), never a bare
// re-derivation from Kconfig alone that would drop a live claim.
//
// The derived mask is then run through bb_wdt_clamp_idle_mask() with this
// target's REAL core count (configNUMBER_OF_CORES, not a hardcoded 2) --
// esp_task_wdt_reconfigure() rejects (ESP_ERR_INVALID_ARG) any
// idle_core_mask bit at or above the real core count, e.g. bit 1 from
// bb_wdt_claim_core(1) on a 1-core esp32-s2/-c3 target. Without this clamp,
// a single out-of-range claim would silently reject every subsequent
// reconfigure -- including unrelated timeout changes -- via the bb_log_w
// warning path below, forever, until the offending claim is released.
static void do_reconfigure(uint32_t timeout_s)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_s * 1000U,
        .idle_core_mask = bb_wdt_clamp_idle_mask(
            bb_wdt_derive_idle_mask(kconfig_default_idle_mask(),
                                     bb_wdt_claimed_core_mask()),
            configNUMBER_OF_CORES),
        .trigger_panic =
#if defined(CONFIG_ESP_TASK_WDT_PANIC) && CONFIG_ESP_TASK_WDT_PANIC
            true,
#else
            false,
#endif
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK) {
        bb_log_w(TAG, "esp_task_wdt_reconfigure(%ums): %s",
                 (unsigned)cfg.timeout_ms, esp_err_to_name(err));
    }
}

void bb_wdt_set_timeout(uint32_t timeout_s)
{
    bb_err_t lock_rc = ensure_timeout_lock();
    // LCOV_EXCL_START -- bb_lock_init() failure is not host-reproducible
    // (this whole platform TU is ESP-IDF-only, not part of the native/host
    // test build; same rationale as bb_wdt_core_claim.c's identical
    // comment). Defensive fallback: still applies the timeout, just without
    // the lock's cross-task guarantee.
    if (lock_rc != BB_OK) {
        s_last_timeout_s = timeout_s;
        do_reconfigure(timeout_s);
        return;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&s_timeout_lock);
    s_last_timeout_s = timeout_s;
    bb_lock_unlock(&s_timeout_lock);
    do_reconfigure(timeout_s);
}

void bb_wdt_priv_reapply(void)
{
    uint32_t timeout_s;
    bb_err_t lock_rc = ensure_timeout_lock();
    // LCOV_EXCL_START -- see bb_wdt_set_timeout()'s identical rationale.
    if (lock_rc != BB_OK) {
        do_reconfigure(s_last_timeout_s);
        return;
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&s_timeout_lock);
    timeout_s = s_last_timeout_s;
    bb_lock_unlock(&s_timeout_lock);
    do_reconfigure(timeout_s);
}

void bb_wdt_extend_begin(uint32_t extended_s)
{
    bb_wdt_set_timeout(extended_s);
}

void bb_wdt_extend_end(void)
{
    bb_wdt_set_timeout(CONFIG_ESP_TASK_WDT_TIMEOUT_S);
}

bb_err_t bb_wdt_task_subscribe(void)
{
    return bb_wdt_task_subscribe_handle(NULL);
}

bb_err_t bb_wdt_task_unsubscribe(void)
{
    return bb_wdt_task_unsubscribe_handle(NULL);
}

bb_err_t bb_wdt_task_subscribe_handle(void *handle)
{
    esp_err_t err = esp_task_wdt_add((TaskHandle_t)handle);
    return (bb_err_t)err;
}

bb_err_t bb_wdt_task_unsubscribe_handle(void *handle)
{
    esp_err_t err = esp_task_wdt_delete((TaskHandle_t)handle);
    if (err == ESP_ERR_NOT_FOUND) {
        return BB_OK;
    }
    return (bb_err_t)err;
}

void bb_wdt_task_feed(void)
{
    esp_task_wdt_reset();
}
