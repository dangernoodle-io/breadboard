#include "bb_wdt.h"

#include "bb_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "bb_wdt";

void bb_wdt_set_timeout(uint32_t timeout_s)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_s * 1000U,
        .idle_core_mask =
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
            (1U << 0) |
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
            (1U << 1) |
#endif
            0U,
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
    esp_err_t err = esp_task_wdt_add(NULL);
    return (bb_err_t)err;
}

bb_err_t bb_wdt_task_unsubscribe(void)
{
    esp_err_t err = esp_task_wdt_delete(NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        return BB_OK;
    }
    return (bb_err_t)err;
}

void bb_wdt_task_feed(void)
{
    esp_task_wdt_reset();
}
