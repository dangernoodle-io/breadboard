#include "bb_wdt.h"
#include "bb_wdt_priv.h"

#include "bb_lock.h"
#include "bb_lock_once.h"
#include "esp_log.h"
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
// runtime claim. MONITORED polarity -- bit N set means core N's idle task
// IS monitored (esp_task_wdt.h: "1 << i means that core i's idle task will
// be monitored by the TWDT") -- mirroring ESP-IDF's own app_startup.c
// computation from these exact same Kconfig symbols. do_reconfigure() below
// converts this ONCE into bb_wdt's internal EXCUSED polarity via
// bb_wdt_invert_core_mask() before feeding bb_wdt_derive_excused_mask(); see
// bb_wdt.h's "Polarity hardening" note (B1-1408).
//
// Boot window: ESP-IDF's app_startup.c inits the Task WDT from these same
// Kconfig symbols (CONFIG_ESP_TASK_WDT_INIT, default y) BEFORE app_main()
// runs -- bb_wdt's own value governs only from its first reconfigure
// onward (the first bb_wdt_set_timeout()/claim/release call). The two agree
// in the nothing-claimed case by construction (same Kconfig symbols, same
// formula); bb_wdt does not own the idle-check mask from boot, only from
// its first reconfigure.
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
// bb_wdt_derive_excused_mask() union (excused seed | claimed), never a bare
// re-derivation from Kconfig alone that would drop a live claim. The whole
// computation happens in EXCUSED polarity (bb_wdt.h's "Polarity hardening"
// note, B1-1408) except the two single-call conversions at each end:
// kconfig_default_idle_mask()'s MONITORED-polarity seed is inverted once on
// the way in, and the final excused mask is inverted back to MONITORED
// polarity exactly once, immediately before it is written into
// esp_task_wdt_config_t.idle_core_mask below.
//
// The excused mask is clamped via bb_wdt_clamp_core_mask() with this
// target's REAL core count (configNUMBER_OF_CORES, not a hardcoded 2) --
// esp_task_wdt_reconfigure() rejects (ESP_ERR_INVALID_ARG) any
// idle_core_mask bit at or above the real core count, e.g. bit 1 from
// bb_wdt_claim_core(1) on a 1-core esp32-s2/-c3 target. Without this clamp,
// a single out-of-range claim would silently reject every subsequent
// reconfigure -- including unrelated timeout changes -- via the ESP_LOGW
// warning path below, forever, until the offending claim is released.
static void do_reconfigure(uint32_t timeout_s)
{
    int num_cores = configNUMBER_OF_CORES;
    bb_wdt_excused_mask_t excused_seed = {
        .bits = bb_wdt_invert_core_mask(kconfig_default_idle_mask(), num_cores)
    };
    // A claim IS excused-polarity by contract (see bb_wdt.h's "Polarity
    // hardening" note) -- named conversion of the bare uint32_t
    // bb_wdt_claimed_core_mask() returns (kept bare there since it is also
    // consumed as a raw ownership bitmask outside bb_wdt, by
    // bb_task_resolve() and bb_http_server's worker steering).
    bb_wdt_excused_mask_t claimed_excused = { .bits = bb_wdt_claimed_core_mask() };
    bb_wdt_excused_mask_t excused = bb_wdt_clamp_core_mask(
        bb_wdt_derive_excused_mask(excused_seed, claimed_excused),
        num_cores);
    // Convert back to MONITORED polarity EXACTLY ONCE, right here -- the
    // only place in bb_wdt that may hold a monitored-polarity value.
    uint32_t monitored = bb_wdt_invert_core_mask(excused.bits, num_cores);

    esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_s * 1000U,
        .idle_core_mask = monitored,
        .trigger_panic =
#if defined(CONFIG_ESP_TASK_WDT_PANIC) && CONFIG_ESP_TASK_WDT_PANIC
            true,
#else
            false,
#endif
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK) {
        // Raw ESP_LOGW, not bb_log_w -- bb_wdt is a floor-safe primitive
        // (mirrors platform/espidf/bb_core/bb_lock.c's identical choice):
        // bb_log's writer task is created via bb_task_create(), and
        // bb_task's own core-claim wiring depends on bb_wdt, so a bb_wdt ->
        // bb_log dependency would close bb_task -> bb_wdt -> bb_log ->
        // bb_task into a component cycle.
        ESP_LOGW(TAG, "esp_task_wdt_reconfigure(%ums): %s",
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
